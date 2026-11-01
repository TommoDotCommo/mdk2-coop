/* MDK2-Coop-Setup.exe: small windowless entry point; the installer UI and logic are readable PowerShell.
 *
 * Two forms of the same program (tools/release.sh builds both):
 *  - the single-file Setup (the main download) carries the release files as resource 10, checks them against the SHA-256 in
 *    resource 12, writes them into a fresh folder %LOCALAPPDATA%\mdk2-coop\setup\<version> and starts installer-gui.ps1 there;
 *  - the copy inside the zip has no payload and starts the scripts beside it. If they are missing (Setup was double-clicked
 *    inside the zip without extracting) it says what to do instead.
 * "Setup.exe /extract <new folder>" writes the payload to a folder and stops: for tests and for anyone who wants to read the
 * scripts first. MDK2_COOP_SETUP_LOG=<file> makes messages go to that file instead of a message box (tests). */
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include "setup-core.h"
#include "../bridge/launcher_update.h"

enum { RES_PAYLOAD = 10, RES_VERSION = 11, RES_SHA256 = 12 };
enum { EXIT_OK = 0, EXIT_FAIL = 1, EXIT_USAGE = 2, EXIT_DAMAGED = 3, EXIT_UNSAFE = 4, EXIT_NO_FILES = 5 };

static void say(const wchar_t *text)
{
    wchar_t log[MAX_PATH];
    if (GetEnvironmentVariableW(L"MDK2_COOP_SETUP_LOG", log, MAX_PATH) && log[0]) {
        HANDLE h = CreateFileW(log, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD n;
            WriteFile(h, text, (DWORD)(wcslen(text) * sizeof(wchar_t)), &n, NULL);
            WriteFile(h, L"\r\n", 4, &n, NULL);
            CloseHandle(h);
        }
        return;
    }
    MessageBoxW(NULL, text, L"MDK2 Co-op Setup", MB_OK | MB_ICONERROR);
}

static const unsigned char *resource(int id, DWORD *size)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    if (!g) return NULL;
    *size = SizeofResource(NULL, r);
    return (const unsigned char *)LockResource(g);
}

/* A folder we are about to trust: a real directory, not a junction or symbolic link. */
static int plain_dir(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return 0;
#ifdef SETUP_MUTANT_FOLLOW_LINKS        /* tools/test_setup_exe.ps1 mutant: links are followed */
    return 1;
#else
    return !(a & FILE_ATTRIBUTE_REPARSE_POINT);
#endif
}

static int ensure_dir(const wchar_t *path)
{
    if (CreateDirectoryW(path, NULL)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS && plain_dir(path);
}

/* Write every payload file below `leaf` (created by us a moment ago). Each file is created new: never over anything. */
static int write_files(const wchar_t *leaf, const struct sc_file *files, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        wchar_t path[MAX_PATH], *p;
        const char *n = files[i].name;
        size_t used = wcslen(leaf), k;
        HANDLE h;
        DWORD wrote = 0, total = 0;
        if (used + 1 + strlen(n) >= MAX_PATH) return EXIT_FAIL;
        wcscpy(path, leaf);
        path[used++] = L'\\';
        for (k = 0; n[k]; k++) path[used + k] = n[k] == '/' ? L'\\' : (wchar_t)n[k];
        path[used + k] = 0;
        for (p = path + used; *p; p++) {
            if (*p == L'\\') {
                *p = 0;
                if (!ensure_dir(path)) return EXIT_UNSAFE;
                *p = L'\\';
            }
        }
        h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (h == INVALID_HANDLE_VALUE) return EXIT_FAIL;
        while (total < files[i].size) {
            if (!WriteFile(h, files[i].data + total, files[i].size - total, &wrote, NULL) || !wrote) { CloseHandle(h); return EXIT_FAIL; }
            total += wrote;
        }
        if (!CloseHandle(h)) return EXIT_FAIL;
    }
    return EXIT_OK;
}

/* Check the embedded payload against its SHA-256 and parse it. */
static int load_payload(const unsigned char **blob, DWORD *len, struct sc_file *files, int *count)
{
    DWORD hashlen = 0;
    const unsigned char *want = resource(RES_SHA256, &hashlen);
    char have[65];
    *blob = resource(RES_PAYLOAD, len);
    if (!*blob || !want) return EXIT_NO_FILES;
#ifndef SETUP_MUTANT_NO_VERIFY          /* tools/test_setup_exe.ps1 mutant: a damaged payload is trusted */
    lu_sha256_hex(*blob, *len, have);
    if (hashlen < 64 || memcmp(have, want, 64) != 0) return EXIT_DAMAGED;
#else
    (void)have;
#endif
    *count = sc_parse(*blob, *len, files);
    return *count < 0 ? EXIT_DAMAGED : EXIT_OK;
}

static void start_gui(const wchar_t *dir, int *code)   /* the Setup exe's own path goes along for the window icon */
{
    wchar_t script[MAX_PATH], powershell[MAX_PATH], self[MAX_PATH], cmd[MAX_PATH * 4];
    STARTUPINFOW startup = { 0 };
    PROCESS_INFORMATION process = { 0 };
    if (wcslen(dir) + 1 + wcslen(L"installer-gui.ps1") >= MAX_PATH) { *code = EXIT_FAIL; return; }
    swprintf(script, MAX_PATH, L"%ls\\installer-gui.ps1", dir);
    if (!GetSystemDirectoryW(powershell, MAX_PATH) || wcslen(powershell) + wcslen(L"\\WindowsPowerShell\\v1.0\\powershell.exe") >= MAX_PATH) {
        *code = EXIT_FAIL; return;
    }
    wcscat(powershell, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) self[0] = 0;
    swprintf(cmd, MAX_PATH * 4, L"\"%ls\" -NoProfile -ExecutionPolicy Bypass -File \"%ls\" -SetupExe \"%ls\"", powershell, script, self);
    startup.cb = sizeof(startup);
    if (!CreateProcessW(powershell, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &startup, &process)) {
        say(L"Could not start the installer. Windows PowerShell is required.");
        *code = EXIT_FAIL;
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

static int exists(const wchar_t *dir, const wchar_t *name)
{
    wchar_t path[MAX_PATH];
    if (wcslen(dir) + 1 + wcslen(name) >= MAX_PATH) return 0;
    swprintf(path, MAX_PATH, L"%ls\\%ls", dir, name);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static int run_beside(const wchar_t *self)
{
    static const wchar_t *need[] = { L"installer-gui.ps1", L"mdk2-coop.ps1", L"install-state.ps1", L"install-policy.exe",
                                     L"dinput8.dll", L"coop\\coop.lua" };
    wchar_t dir[MAX_PATH], temp[MAX_PATH], longself[MAX_PATH], longtemp[MAX_PATH], *end;
    size_t i;
    int missing = 0, code = EXIT_OK;
    wcscpy(dir, self);
    if (!(end = wcsrchr(dir, L'\\'))) return EXIT_FAIL;
    *end = 0;
    for (i = 0; i < sizeof(need) / sizeof(*need); i++) if (!exists(dir, need[i])) missing = 1;
    wcscpy(longself, self);
    GetLongPathNameW(self, longself, MAX_PATH);
    temp[0] = 0;
    if (GetTempPathW(MAX_PATH, temp)) { wcscpy(longtemp, temp); GetLongPathNameW(temp, longtemp, MAX_PATH); } else longtemp[0] = 0;
    if (missing || (longtemp[0] && sc_run_from_zip(longself, longtemp))) {
        say(L"Setup cannot find the files that belong with it.\n\nExtract the whole zip first (right-click the zip, Extract All, "
            L"then open the extracted folder), or download the single MDK2-Coop-Setup.exe from the release page: "
            L"that one needs no extracting.");
        return EXIT_NO_FILES;
    }
    start_gui(dir, &code);
    return code;
}

static int run_payload(const wchar_t *explicit_leaf)
{
    const unsigned char *blob, *ver;
    DWORD len, verlen = 0;
    struct sc_file files[SC_MAX_FILES];
    int count = 0, code = load_payload(&blob, &len, files, &count);
    wchar_t leaf[MAX_PATH], version[64];
    size_t i;
    if (code == EXIT_DAMAGED) {
        say(L"This Setup file is damaged (its contents do not match their checksum). Download MDK2-Coop-Setup.exe again.");
        return code;
    }
    if (code) return code;
    if (explicit_leaf) {
        if (wcslen(explicit_leaf) >= MAX_PATH - 100 || !CreateDirectoryW(explicit_leaf, NULL)) return EXIT_FAIL;
        wcscpy(leaf, explicit_leaf);
        return write_files(leaf, files, count);
    }
    ver = resource(RES_VERSION, &verlen);
    for (i = 0; ver && i < verlen && i < 63 && ver[i]; i++) version[i] = (wchar_t)ver[i];
    version[ver ? i : 0] = 0;
    if (!version[0] || wcschr(version, L'\\') || wcschr(version, L'/') || !wcscmp(version, L".") || !wcscmp(version, L"..")) wcscpy(version, L"setup");
    {
        wchar_t base[MAX_PATH];
        int n;
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH - 120) || !base[0]) { say(L"Setup could not find your user profile folder (LOCALAPPDATA)."); return EXIT_FAIL; }
        if (!plain_dir(base)) { say(L"Your user profile folder is not a normal folder; Setup stopped."); return EXIT_UNSAFE; }
        wcscat(base, L"\\mdk2-coop");
        if (!ensure_dir(base)) { say(L"Setup stopped: the mdk2-coop folder in your profile is a link or cannot be created."); return EXIT_UNSAFE; }
        wcscat(base, L"\\setup");
        if (!ensure_dir(base)) { say(L"Setup stopped: the mdk2-coop\\setup folder in your profile is a link or cannot be created."); return EXIT_UNSAFE; }
        /* a fresh folder every run: exclusive create, so nothing already there is ever reused or overwritten */
        for (n = 1; n < 100; n++) {
            if (n == 1) swprintf(leaf, MAX_PATH, L"%ls\\%ls", base, version);
            else swprintf(leaf, MAX_PATH, L"%ls\\%ls-%d", base, version, n);
            if (CreateDirectoryW(leaf, NULL)) break;
            if (GetLastError() != ERROR_ALREADY_EXISTS) { say(L"Setup could not create its working folder in your profile."); return EXIT_FAIL; }
        }
        if (n >= 100) { say(L"Setup found too many old working folders in %LOCALAPPDATA%\\mdk2-coop\\setup. Delete them and try again."); return EXIT_FAIL; }
    }
    code = write_files(leaf, files, count);
    if (code) { say(L"Setup could not unpack its files into your profile folder."); return code; }
    start_gui(leaf, &code);
    return code;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    wchar_t self[MAX_PATH];
    DWORD size = 0;
    int argc = 0, hasPayload;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)instance; (void)previous; (void)command; (void)show;
    hasPayload = resource(RES_PAYLOAD, &size) != NULL;
    if (argv && argc == 3 && !wcscmp(argv[1], L"/extract")) return hasPayload ? run_payload(argv[2]) : EXIT_NO_FILES;
    if (argc != 1) return EXIT_USAGE;
    if (hasPayload) return run_payload(NULL);
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return EXIT_FAIL;
    return run_beside(self);
}
