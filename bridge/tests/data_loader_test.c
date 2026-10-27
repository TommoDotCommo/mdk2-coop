/* Load the real bridge from a disposable fixture without starting the game. */
#include <windows.h>
int main(void)
{
    HMODULE bridge = LoadLibraryA("dinput8.dll");
    if (!bridge) return 1;
    FreeLibrary(bridge);
    return 0;
}
