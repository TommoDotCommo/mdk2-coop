/* A stand-in for another mod's dinput8.dll (tools/test_chain_load.py, tools/qa_chain_live.sh), installed as
 * dinput8.chain.dll. It records being loaded (chain_loaded) and, built with -DFULL_PROXY, is a full dinput8 proxy itself:
 * DirectInput8Create counts its calls (chain_hits) and forwards to the system dinput8, as an ASI loader's proxy does.
 * With -DHIT_FILE each call also rewrites chain-hits.txt beside the DLL (the live test reads it; a fixture only). */
#include <windows.h>
#include <string.h>

__declspec(dllexport) volatile LONG chain_loaded, chain_hits;

#ifdef FULL_PROXY
typedef HRESULT (WINAPI *create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    char path[MAX_PATH];
    HMODULE sys;
    create_t fn;
    InterlockedIncrement(&chain_hits);
#ifdef HIT_FILE
    {
        HMODULE self = NULL;
        char file[MAX_PATH], *slash, text[16];
        DWORD n;
        HANDLE h;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&chain_hits, &self);
        GetModuleFileNameA(self, file, MAX_PATH);
        if ((slash = strrchr(file, '\\'))) lstrcpyA(slash + 1, "chain-hits.txt");
        h = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            wsprintfA(text, "%ld", chain_hits);
            WriteFile(h, text, lstrlenA(text), &n, NULL);
            CloseHandle(h);
        }
    }
#endif
    GetSystemDirectoryA(path, MAX_PATH);
    lstrcatA(path, "\\dinput8.dll");
    sys = LoadLibraryA(path);
    fn = sys ? (create_t)GetProcAddress(sys, "DirectInput8Create") : NULL;
    return fn ? fn(inst, ver, riid, out, outer) : E_FAIL;
}
#endif

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        InterlockedExchange(&chain_loaded, 1);
    return TRUE;
}
