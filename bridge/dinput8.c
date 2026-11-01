/*
 * mdk2-coop bridge: dinput8.dll proxy loaded by mdk2hd.exe.
 *
 * Forwards DirectInput to the system DLL and hosts the co-op code: it repoints an
 * unused Lua builtin to coop_init (lua.c) before the game starts Lua. The bridge
 * checks the build before touching anything.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "bridge.h"
#include "crash_context.h"
#include "build_sigs.h"
#include "launcher_update.h"

void l_coop_init(void);
void loader_ensure(void);      /* loader.c */

/* Lua builtin table {name, fn} entry for "copytagmethods", unused by the
 * game's scripts. Repointed to coop_init before Lua starts. */
#define BUILTIN_ENTRY      ((void **)ADDR.builtin_entry)
#define BUILTIN_ENTRY_NAME ((const char *)ADDR.builtin_name)

/* ---- log ---- */

static FILE *log_file;
char coop_dir[MAX_PATH], game_dir[MAX_PATH];

void logmsg(const char *fmt, ...)
{
    va_list ap;
    char line[1100];

    if (!log_file)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fputs(line, log_file);
    fputc('\n', log_file);
    fflush(log_file);
    diag_log_line(line);        /* the session log (never replaced) and the flight recorder's tail */
}

/* A crash: the exception, where (module + offset) and what was touched go in bridge.log, so a report has it.
 * A vectored handler, because the game or Steam may replace the unhandled-exception filter. It sees first-chance
 * exceptions too, so it logs only the kinds that end the game, and only the first few. */
static LONG CALLBACK crash_logger(EXCEPTION_POINTERS *e)
{
    static int logged;
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    DWORD code = r->ExceptionCode;
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?", *base;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (logged++ >= 5)
        return EXCEPTION_CONTINUE_SEARCH;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)r->ExceptionAddress, &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    base = strrchr(name, '\\');
    logmsg("crash: exception 0x%08lx at %s+0x%lx (address 0x%p, %s 0x%p)", code, base ? base + 1 : name,
           (unsigned long)((char *)r->ExceptionAddress - (char *)mod), r->ExceptionAddress,
           r->NumberParameters >= 2 && r->ExceptionInformation[0] ? "writing" : "reading",
           r->NumberParameters >= 2 ? (void *)r->ExceptionInformation[1] : NULL);
    crash_log_context(e->ContextRecord);
    diag_crash();               /* marks the session as crashed and writes the flight recorder */
    return EXCEPTION_CONTINUE_SEARCH;
}

struct build_addrs ADDR;
static void *crash_handler;

static const struct build_addrs BUILDS[] = {
    { COOP_BUILD_STEAM,
      0x476bb0, 0x476c40, 0x476b80, 0x476690, 0x476a20, 0x476a60,
      0x478100, 0x478de0, 0x476ce0, 0x477980, 0x475700, 0x4776b0,
      0x457520, 0x458360,
      0x4f64a8, 0x524e10, 0x524378, 0x524380, 0x5494c8, 0x549500 },
    { COOP_BUILD_OVERHAUL,
      0x476710, 0x4767a0, 0x4766e0, 0x4761f0, 0x476580, 0x4765c0,
      0x477c60, 0x478940, 0x476840, 0x4774e0, 0x475260, 0x477210,
      0x457500, 0x458340,
      0x4f54a8, 0x523e10, 0x523378, 0x523380, 0x548488, 0x5484c0 },
};

const char *build_name(void) { return ADDR.name ? ADDR.name : "unknown"; }

/* Memory the process can read now, tested without raising an exception: a probe of an address this process does not have
 * (any exe but the known builds) must not be logged as a crash in bridge.log, and must not run the exception handlers. */
static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    return (const char *)p + n <= (const char *)mi.BaseAddress + mi.RegionSize;
}

/* The builtin table entry { "copytagmethods", fn } is where the Lua hook goes, and it identifies the build. */
static int select_build(void)
{
    unsigned i;

    for (i = 0; i < sizeof(BUILDS) / sizeof(BUILDS[0]); i++) {
        void **entry = (void **)BUILDS[i].builtin_entry;
        const char *name = (const char *)BUILDS[i].builtin_name;

        if (readable(entry, 8) && readable(name, 15) && entry[0] == (void *)name &&
            strcmp(name, "copytagmethods") == 0) {
            ADDR = BUILDS[i];
            return 1;
        }
    }
    return 0;
}

/* Without a known build the Lua hook cannot go in, so neither can the launcher: say why in plain words. The text goes to
 * a file next to the log (coop\\unsupported-build.txt) and, outside background test mode, to a message box. Both happen on
 * a thread of their own, started from the game's first DirectInput8Create call (DllMain holds the loader lock, and the box
 * must not block the game's start). */
#define UNSUPPORTED_BUILD_TEXT \
    "Co-op is off: this copy of MDK2 HD is a game build the co-op mod does not know.\r\n\r\n" \
    "Supported: MDK2 HD on Steam (build 3902) and the Overhaul Games build.\r\n\r\n" \
    "What to do: in Steam, right-click MDK2 HD > Properties > Betas and choose None (opt out of betas), then " \
    "Installed Files > Verify integrity of game files, and start the game again. The game itself still plays normally.\r\n"

static DWORD WINAPI unsupported_build_notice(LPVOID self)
{
    char text[1200], hash[65] = "", ini[MAX_PATH], exe[MAX_PATH], file[MAX_PATH];
    FILE *f;

#ifdef COOP_MUTANT_NO_BUILD_NOTICE      /* tools/qa_build_message.sh MUTANT=silent: the old behaviour, only a log line */
    FreeLibraryAndExitThread((HMODULE)self, 0);
#endif
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (!lu_file_sha256(exe, hash))
        strcpy(hash, "unreadable");
    snprintf(text, sizeof(text), "%sGame file SHA-256: %s\r\n", UNSUPPORTED_BUILD_TEXT, hash);
    snprintf(file, MAX_PATH, "%s\\unsupported-build.txt", coop_dir);
    if ((f = fopen(file, "wb"))) {
        fputs(text, f);
        fclose(f);
    }
    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    /* background test games stay off the desktop; [test] notice_dialog=1 shows the box anyway, without taking focus
     * (tools/qa_build_message.sh finds it and closes it) */
    if (!GetPrivateProfileIntA("test", "background", 0, ini))
        MessageBoxA(NULL, text, "MDK2 Co-op", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    else if (GetPrivateProfileIntA("test", "notice_dialog", 0, ini))
        MessageBoxA(NULL, text, "MDK2 Co-op", MB_OK | MB_ICONWARNING);
    FreeLibraryAndExitThread((HMODULE)self, 0);     /* the thread holds a reference to this DLL until it is done (the updater test unloads it) */
    return 0;
}

static volatile LONG unsupported_build;     /* set by install_lua_hook when select_build fails */

/* The game's first DirectInput8Create call: it runs on the game's own thread, after DllMain. */
static void announce_unsupported_build(void)
{
    HANDLE t = NULL;
    HMODULE self = NULL;

    char exe[MAX_PATH], *name;

    if (!unsupported_build || InterlockedExchange(&unsupported_build, 0) == 0)
        return;
    /* only the game itself: a test harness or tool that loads this DLL is not "an unknown build of MDK2 HD" */
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    name = strrchr(exe, '\\');
    if (_stricmp(name ? name + 1 : exe, "mdk2hd.exe") != 0)
        return;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)unsupported_build_notice, &self))   /* takes a reference */
        t = CreateThread(NULL, 0, unsupported_build_notice, self, 0, NULL);
    if (t)
        CloseHandle(t);
    else if (self)
        FreeLibrary(self);
}

static int install_lua_hook(void)
{
    void **entry;
    DWORD old;

    if (!select_build()) {
        logmsg("unknown game build: lua hook not installed (co-op is off; see unsupported-build.txt)");
        unsupported_build = 1;      /* said to the player when the game first asks for DirectInput (not here: DllMain holds the loader lock) */
        return 0;
    }
    logmsg("game build: %s", ADDR.name);
    {   /* a notice left by an earlier run on another build is stale now */
        char stale[MAX_PATH];
        snprintf(stale, MAX_PATH, "%s\\unsupported-build.txt", coop_dir);
        DeleteFileA(stale);
    }
    entry = BUILTIN_ENTRY;

    VirtualProtect(entry, 8, PAGE_READWRITE, &old);
    entry[0] = (void *)"coop_init";
    entry[1] = (void *)l_coop_init;
    VirtualProtect(entry, 8, old, &old);
    logmsg("lua hook installed");
    return 1;
}

/* ---- dinput8 forwarding ---- */

static HMODULE real_dinput8;

static FARPROC real_proc(const char *name)
{
    if (!real_dinput8) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\dinput8.dll");
        real_dinput8 = LoadLibraryA(path);
        logmsg("loaded %s -> %p", path, (void *)real_dinput8);
    }
    return real_dinput8 ? GetProcAddress(real_dinput8, name) : NULL;
}

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    DirectInput8Create_t fn = (DirectInput8Create_t)real_proc("DirectInput8Create");
    announce_unsupported_build();
    HRESULT hr = fn ? fn(inst, ver, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr))
        input_wrap_dinput(*out);
    return hr;
}

HRESULT WINAPI Proxy_DllCanUnloadNow(void)
{
    FARPROC fn = real_proc("DllCanUnloadNow");
    return fn ? ((HRESULT (WINAPI *)(void))fn)() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *out)
{
    FARPROC fn = real_proc("DllGetClassObject");
    return fn ? ((HRESULT (WINAPI *)(REFCLSID, REFIID, LPVOID *))fn)(clsid, riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_DllRegisterServer(void)
{
    FARPROC fn = real_proc("DllRegisterServer");
    return fn ? ((HRESULT (WINAPI *)(void))fn)() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer(void)
{
    FARPROC fn = real_proc("DllUnregisterServer");
    return fn ? ((HRESULT (WINAPI *)(void))fn)() : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH], *slash;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(NULL, game_dir, MAX_PATH);
        slash = strrchr(game_dir, '\\');
        if (slash)
            *slash = 0;
        snprintf(coop_dir, MAX_PATH, "%s\\coop", game_dir);
        CreateDirectoryA(coop_dir, NULL);
        {   /* Player installs prefer the user profile; test and couch copies keep their own data. */
            char probe[MAX_PATH], base[MAX_PATH], marker[MAX_PATH];
            FILE *t;
            int per_user;
            DWORD n;
            snprintf(marker, MAX_PATH, "%s\\per-user-data", coop_dir);
            per_user = GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES;
            snprintf(probe, MAX_PATH, "%s\\write.test", coop_dir);
            t = per_user ? NULL : fopen(probe, "w");
            if (t) {
                fclose(t);
                DeleteFileA(probe);
            } else if ((n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH)) > 0 && n < MAX_PATH - 16) {
                strcat(base, "\\mdk2-coop");
                CreateDirectoryA(base, NULL);
                snprintf(coop_dir, MAX_PATH, "%s\\coop", base);
                CreateDirectoryA(coop_dir, NULL);
            }
        }
        snprintf(path, MAX_PATH, "%s\\bridge.log", coop_dir);
        {   /* the last run's log is kept: after a crash, starting again must not wipe the evidence */
            char previous[MAX_PATH];
            snprintf(previous, MAX_PATH, "%s\\bridge.previous.log", coop_dir);
            MoveFileExA(path, previous, MOVEFILE_REPLACE_EXISTING);
        }
        log_file = fopen(path, "w");
        if (!log_file)
            OutputDebugStringA("mdk2-coop: cannot open bridge.log; check permissions and free disk space\n");
        diag_log_open();
        logmsg("mdk2-coop bridge attached");
        logmsg("files in %s", coop_dir);
        crash_handler = AddVectoredExceptionHandler(1, crash_logger);
        redirect_init();
        if (install_lua_hook())
            loader_ensure();
        input_init();
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        if (crash_handler)      /* unloaded with FreeLibrary (the updater tests do): the handler must not outlive this code */
            RemoveVectoredExceptionHandler(crash_handler);
    } else if (reason == DLL_PROCESS_DETACH && reserved) {
        diag_log_close();           /* a session log without this line ended in a crash or a kill */
    }
    return TRUE;
}
