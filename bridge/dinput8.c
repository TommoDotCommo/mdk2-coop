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
static int instance_data;           /* 1: a test or couch copy keeps its data in <game>\coop */
static char moved_note[MAX_PATH * 2 + 48];

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

/* ---- dinput8 forwarding and the chained dinput8 ---- */

/* Another mod's dinput8.dll (an ASI loader, a widescreen fix) that Setup found in the game folder is kept as
 * dinput8.chain.dll and loaded here, at the end of this DLL's own start-up. When it is a full dinput8 proxy itself (it
 * exports DirectInput8Create) the game's DirectInput calls go through it, so it keeps working as before; otherwise it is a
 * plug-in loaded for its side effects and the exports go to the system dinput8. Never chained: a copy of this mod. */
static HMODULE real_dinput8, chain_dll;
static DWORD chain_thread;      /* the thread inside the chained DirectInput8Create: a proxy that calls back here goes to the system */

static void chain_load(void)
{
    char path[MAX_PATH];
    unsigned char *bytes = NULL;
    size_t len = 0, i;
    int ours = 0;

    snprintf(path, MAX_PATH, "%s\\dinput8.chain.dll", game_dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        logmsg("chain: none (no dinput8.chain.dll)");
        return;
    }
#ifdef COOP_MUTANT_NO_CHAIN     /* tools/test_chain_load.py MUTANT=ignored: the old behaviour, the file is never loaded */
    logmsg("chain: %s ignored", path);
    return;
#endif
    if (lu_read_bytes(path, &bytes, &len)) {
        static const char mark[] = "mdk2-coop bridge attached";
        for (i = 0; !ours && len >= sizeof(mark) && i <= len - sizeof(mark); i++)
            ours = bytes[i] == 'm' && !memcmp(bytes + i, mark, sizeof(mark) - 1);
        free(bytes);
    }
    if (ours) {
        logmsg("chain: %s is a copy of this mod: not loaded", path);
        return;
    }
    chain_dll = LoadLibraryA(path);
    if (!chain_dll) {
        logmsg("chain: %s could not be loaded (error %lu)", path, GetLastError());
        return;
    }
    logmsg("chain: loaded %s -> %p (%s)", path, (void *)chain_dll, GetProcAddress(chain_dll, "DirectInput8Create")
           ? "a full dinput8 proxy: DirectInput goes through it" : "a plug-in: DirectInput goes to the system dinput8");
}

static FARPROC real_proc(const char *name)
{
    FARPROC fn;
    if (chain_dll && chain_thread != GetCurrentThreadId() && (fn = GetProcAddress(chain_dll, name)))
        return fn;
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
    int nested = chain_dll && chain_thread == GetCurrentThreadId();    /* the chained proxy calling back into this DLL */
    int via_chain = chain_dll && !nested && (FARPROC)fn == GetProcAddress(chain_dll, "DirectInput8Create");
    HRESULT hr;
    announce_unsupported_build();
    if (via_chain)
        chain_thread = GetCurrentThreadId();
    hr = fn ? fn(inst, ver, riid, out, outer) : E_FAIL;
    if (via_chain)
        chain_thread = 0;
    if (SUCCEEDED(hr) && !nested)     /* wrapped once, by the outer call */
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

/* ---- where the co-op data lives ----
 * Nothing is written into the game folder at run time: settings, logs, diagnostics, update backups and markers go to
 * %LOCALAPPDATA%\mdk2-coop\coop. One exception, for test and couch copies: a coop.ini that a tool placed in <game>\coop
 * with a [test] or [couch] section keeps that copy's data in <game>\coop, as before (the mod never creates that file).
 * Data an older version wrote into <game>\coop is moved to the profile once (top-level files, never overwriting). */
static int ini_has_section(const char *ini, const char *section)
{
    char buf[64];
    return GetPrivateProfileSectionA(section, buf, sizeof(buf), ini) > 0;
}

static void move_old_game_data(const char *from_dir)
{
    char pattern[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int moved = 0;

    snprintf(pattern, MAX_PATH, "%s\\*", from_dir);
    if ((h = FindFirstFileA(pattern, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        const char *n = fd.cFileName;
        size_t len = strlen(n);
        char a[65], b[65];
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        /* the package's files, Setup's and install-linux.sh's bookkeeping, and the old updater's (lu_startup moves those) */
        if (!_stricmp(n, "coop.lua") || !_stricmp(n, "per-user-data") || !_stricmp(n, "install-linux.record") ||
            !_stricmp(n, "update-state.ini") || !_stricmp(n, "coop-mod.json") || strstr(n, ".coop-") ||
            (len > 9 && !_stricmp(n + len - 9, ".previous")) || strstr(n, "-stage"))
            continue;
        snprintf(src, MAX_PATH, "%s\\%s", from_dir, n);
        snprintf(dst, MAX_PATH, "%s\\%s", coop_dir, n);
        if (GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES)
            continue;                       /* the profile's own copy wins; the old one stays where it is */
        if (CopyFileA(src, dst, TRUE) && lu_file_sha256(src, a) && lu_file_sha256(dst, b) && !strcmp(a, b) && DeleteFileA(src))
            moved++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (moved)
        snprintf(moved_note, sizeof(moved_note), "moved %d file(s) from %s to %s", moved, from_dir, coop_dir);
}

static void choose_coop_dir(void)
{
    char game_coop[MAX_PATH], ini[MAX_PATH], base[MAX_PATH];
    DWORD n;

    snprintf(game_coop, MAX_PATH, "%s\\coop", game_dir);
    snprintf(ini, MAX_PATH, "%s\\coop.ini", game_coop);
    if (GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES && (ini_has_section(ini, "test") || ini_has_section(ini, "couch"))) {
        snprintf(coop_dir, MAX_PATH, "%s", game_coop);
        instance_data = 1;
        return;
    }
    n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 20) {     /* no profile folder at all: the old place */
        snprintf(coop_dir, MAX_PATH, "%s", game_coop);
        CreateDirectoryA(coop_dir, NULL);
        return;
    }
    strcat(base, "\\mdk2-coop");
    CreateDirectoryA(base, NULL);
    snprintf(coop_dir, MAX_PATH, "%s\\coop", base);
    CreateDirectoryA(coop_dir, NULL);
    move_old_game_data(game_coop);
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
        choose_coop_dir();
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
        logmsg("files in %s%s", coop_dir, instance_data ? " (this copy's own coop.ini: test or couch copy)" : "");
        if (moved_note[0])
            logmsg("data of an older version: %s", moved_note);
        crash_handler = AddVectoredExceptionHandler(1, crash_logger);
        redirect_init();
        if (install_lua_hook())
            loader_ensure();
        input_init();
        chain_load();
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        if (crash_handler)      /* unloaded with FreeLibrary (the updater tests do): the handler must not outlive this code */
            RemoveVectoredExceptionHandler(crash_handler);
    } else if (reason == DLL_PROCESS_DETACH && reserved) {
        diag_log_close();           /* a session log without this line ended in a crash or a kill */
    }
    return TRUE;
}
