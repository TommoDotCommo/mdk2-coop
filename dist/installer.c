/* Small windowless entry point; the installer UI and logic are readable PowerShell. */
#include <windows.h>
#include <wchar.h>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    wchar_t path[MAX_PATH], args[MAX_PATH * 2], *end;
    STARTUPINFOW startup = { 0 };
    PROCESS_INFORMATION process = { 0 };
    wchar_t powershell[MAX_PATH], commandLine[MAX_PATH * 4];
    (void)instance; (void)previous; (void)command; (void)show;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH) || !(end = wcsrchr(path, L'\\'))) return 1;
    if ((size_t)(end - path) + 1 + wcslen(L"installer-gui.ps1") >= MAX_PATH) return 1;
    wcscpy(end + 1, L"installer-gui.ps1");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"The installer files are missing. Extract the entire downloaded zip and try again.", L"MDK2 Co-op", MB_OK | MB_ICONERROR);
        return 1;
    }
    _snwprintf(args, sizeof(args) / sizeof(*args), L"-NoProfile -ExecutionPolicy Bypass -File \"%ls\"", path);
    args[sizeof(args) / sizeof(*args) - 1] = 0;
    if (!GetSystemDirectoryW(powershell, MAX_PATH) ||
        wcslen(powershell) + wcslen(L"\\WindowsPowerShell\\v1.0\\powershell.exe") >= MAX_PATH) return 1;
    wcscat(powershell, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    _snwprintf(commandLine, sizeof(commandLine) / sizeof(*commandLine), L"\"%ls\" %ls", powershell, args);
    commandLine[sizeof(commandLine) / sizeof(*commandLine) - 1] = 0;
    startup.cb = sizeof(startup);
    if (!CreateProcessW(powershell, commandLine, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        MessageBoxW(NULL, L"Could not start the installer. Windows PowerShell is required.", L"MDK2 Co-op", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
