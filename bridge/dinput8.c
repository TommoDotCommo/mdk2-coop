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
    if (!log_file)
        return;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    fflush(log_file);
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
    return EXCEPTION_CONTINUE_SEARCH;
}

struct build_addrs ADDR;

static const struct build_addrs BUILDS[] = {
    { "Steam build 3902", 0x506520, 0x4d7d34,
      0x476bb0, 0x476c40, 0x476b80, 0x476690, 0x476a20, 0x476a60,
      0x478100, 0x478de0, 0x476ce0, 0x477980, 0x475700, 0x4776b0,
      0x457520, 0x458360,
      0x4f64a8, 0x524e10, 0x524378, 0x524380, 0x5494c8, 0x549500 },
    { "Overhaul Games build", 0x505510, 0x4d769c,
      0x476710, 0x4767a0, 0x4766e0, 0x4761f0, 0x476580, 0x4765c0,
      0x477c60, 0x478940, 0x476840, 0x4774e0, 0x475260, 0x477210,
      0x457500, 0x458340,
      0x4f54a8, 0x523e10, 0x523378, 0x523380, 0x548488, 0x5484c0 },
};

const char *build_name(void) { return ADDR.name ? ADDR.name : "unknown"; }

/* The builtin table entry { "copytagmethods", fn } is where the Lua hook goes, and it identifies the build. */
static int select_build(void)
{
    unsigned i;

    for (i = 0; i < sizeof(BUILDS) / sizeof(BUILDS[0]); i++) {
        void **entry = (void **)BUILDS[i].builtin_entry;
        const char *name = (const char *)BUILDS[i].builtin_name;

        if (!IsBadReadPtr(entry, 8) && !IsBadReadPtr(name, 15) && entry[0] == (void *)name &&
            strcmp(name, "copytagmethods") == 0) {
            ADDR = BUILDS[i];
            return 1;
        }
    }
    return 0;
}

static int install_lua_hook(void)
{
    void **entry;
    DWORD old;

    if (!select_build()) {
        logmsg("unknown game build: lua hook not installed");
        return 0;
    }
    logmsg("game build: %s", ADDR.name);
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
        {   /* Program Files is read-only without admin rights: keep the mod's files somewhere writable */
            char probe[MAX_PATH], base[MAX_PATH];
            FILE *t;
            snprintf(probe, MAX_PATH, "%s\\write.test", coop_dir);
            if ((t = fopen(probe, "w"))) {
                fclose(t);
                DeleteFileA(probe);
            } else if (GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH)) {
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
        logmsg("mdk2-coop bridge attached");
        logmsg("files in %s", coop_dir);
        AddVectoredExceptionHandler(1, crash_logger);
        if (install_lua_hook())
            loader_ensure();
        input_init();
    }
    return TRUE;
}
