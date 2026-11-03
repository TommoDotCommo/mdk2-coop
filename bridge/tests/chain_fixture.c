/* A stand-in for another mod's dinput8.dll (tools/test_chain_load.py), installed as dinput8.chain.dll. It records being
 * loaded (chain_loaded) and, built with -DFULL_PROXY, is a full dinput8 proxy itself: DirectInput8Create counts its calls
 * (chain_hits) and forwards to the system dinput8, as an ASI loader's proxy does. */
#include <windows.h>

__declspec(dllexport) volatile LONG chain_loaded, chain_hits;

#ifdef FULL_PROXY
typedef HRESULT (WINAPI *create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    char path[MAX_PATH];
    HMODULE sys;
    create_t fn;
    InterlockedIncrement(&chain_hits);
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
