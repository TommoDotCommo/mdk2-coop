/* Small windowless entry point; the installer UI and logic are readable PowerShell. */
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    wchar_t path[MAX_PATH], args[MAX_PATH * 2], *end;
    SHELLEXECUTEINFOW run = { 0 };
    (void)instance; (void)previous; (void)command; (void)show;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH) || !(end = wcsrchr(path, L'\\'))) return 1;
    if ((size_t)(end - path) + 1 + wcslen(L"installer-gui.ps1") >= MAX_PATH) return 1;
    wcscpy(end + 1, L"installer-gui.ps1");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"The installer files are missing. Extract the entire downloaded zip and try again.", L"MDK2 Co-op", MB_OK | MB_ICONERROR);
        return 1;
    }
    _snwprintf(args, sizeof(args) / sizeof(*args), L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%ls\"", path);
    args[sizeof(args) / sizeof(*args) - 1] = 0;
    run.cbSize = sizeof(run); run.fMask = SEE_MASK_NOCLOSEPROCESS;
    run.lpFile = L"powershell.exe"; run.lpParameters = args; run.nShow = SW_HIDE;
    if (!ShellExecuteExW(&run)) {
        MessageBoxW(NULL, L"Could not start the installer. Windows PowerShell is required.", L"MDK2 Co-op", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(run.hProcess);
    return 0;
}
