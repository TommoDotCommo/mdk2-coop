/* Loads the mod's dinput8.dll from this exe's folder the way the game does, asks it for DirectInput, and prints what the
 * chained DLL saw: "chain=<0|1> hr=<hex> hits=<n>" (tools/test_chain_load.py). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <dinput.h>

typedef HRESULT (WINAPI *create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

int main(void)
{
    char dir[MAX_PATH], path[MAX_PATH], *slash;
    HMODULE proxy, chain;
    create_t create;
    void *di = NULL;
    HRESULT hr;
    volatile LONG *hits;

    GetModuleFileNameA(NULL, dir, MAX_PATH);
    slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    snprintf(path, MAX_PATH, "%s\\dinput8.dll", dir);
    proxy = LoadLibraryA(path);
    if (!proxy) { printf("proxy-load-failed %lu\n", GetLastError()); return 2; }
    create = (create_t)GetProcAddress(proxy, "DirectInput8Create");
    if (!create) { puts("no-export"); return 2; }
    hr = create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, &di, NULL);
    chain = GetModuleHandleA("dinput8.chain.dll");
    hits = chain ? (volatile LONG *)GetProcAddress(chain, "chain_hits") : NULL;
    printf("chain=%d hr=%08lx hits=%ld\n", chain != NULL, (unsigned long)hr, hits ? *hits : -1L);
    fflush(stdout);
    if (SUCCEEDED(hr) && di)
        ((IUnknown *)di)->lpVtbl->Release((IUnknown *)di);
    ExitProcess(0);     /* not FreeLibrary: the proxy stays mapped, as in the game */
}
