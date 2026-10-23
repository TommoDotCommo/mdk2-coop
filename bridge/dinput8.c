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
#define BUILTIN_ENTRY      ((void **)0x506520)
#define BUILTIN_ENTRY_NAME ((const char *)0x4d7d34)

/* ---- log ---- */

static FILE *log_file;
char coop_dir[MAX_PATH];

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

static int install_lua_hook(void)
{
    void **entry = BUILTIN_ENTRY;
    DWORD old;

    if (IsBadReadPtr(entry, 8) || IsBadReadPtr(BUILTIN_ENTRY_NAME, 15) ||
        entry[0] != (void *)BUILTIN_ENTRY_NAME || strcmp(BUILTIN_ENTRY_NAME, "copytagmethods") != 0) {
        logmsg("unknown game build: lua hook not installed");
        return 0;
    }
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
        GetModuleFileNameA(NULL, coop_dir, MAX_PATH);
        slash = strrchr(coop_dir, '\\');
        if (slash)
            *slash = 0;
        strcat(coop_dir, "\\coop");
        CreateDirectoryA(coop_dir, NULL);
        snprintf(path, MAX_PATH, "%s\\bridge.log", coop_dir);
        {   /* the last run's log is kept: after a crash, starting again must not wipe the evidence */
            char previous[MAX_PATH];
            snprintf(previous, MAX_PATH, "%s\\bridge.previous.log", coop_dir);
            MoveFileExA(path, previous, MOVEFILE_REPLACE_EXISTING);
        }
        log_file = fopen(path, "w");
        logmsg("mdk2-coop bridge attached");
        AddVectoredExceptionHandler(1, crash_logger);
        if (install_lua_hook())
            loader_ensure();
        input_init();
    }
    return TRUE;
}
