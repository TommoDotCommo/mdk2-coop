/*
 * Per-instance user data folder ([test] save_dir in coop.ini, off by default).
 * The game finds %LOCALAPPDATA% through SHGetSpecialFolderPathA(CSIDL_LOCAL_APPDATA) and keeps
 * its saves, crash reports, screenshots and settings.lua in <that>\MDK2HD. The environment variable
 * does not move it. With save_dir set, the game exe's import of that one function is pointed at a
 * wrapper that answers with the instance folder, so every copy of the game writes beside itself and
 * never into the real profile. Both supported exes import the function the same way. Players leave
 * the key unset: nothing is hooked then. The seed (a known-good save set) is copied by tools/mdk.sh.
 */
#include <stdio.h>
#include <string.h>
#include "bridge.h"
#include <shlobj.h>

static char base[MAX_PATH];     /* the folder that stands in for %LOCALAPPDATA%; empty: redirect off */
static int installed, calls;
static BOOL (WINAPI *real_special)(HWND, LPSTR, int, BOOL);

static void make_dirs(const char *path)
{
    char tmp[MAX_PATH], *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 3; *p; p++)
        if (*p == '\\') {
            *p = 0;
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    CreateDirectoryA(tmp, NULL);
}

static BOOL WINAPI special_folder(HWND wnd, LPSTR out, int csidl, BOOL create)
{
    if (out && (csidl & 0xff) == CSIDL_LOCAL_APPDATA && strlen(base) < MAX_PATH) {
        if (create)
            make_dirs(base);
        strcpy(out, base);
        if (calls++ < 4)
            logmsg("redirect: CSIDL_LOCAL_APPDATA -> %s", base);
        return TRUE;
    }
    return real_special(wnd, out, csidl, create);
}

/* Point the module's import of dll!func at hook. 1 patched, 0 not imported, -1 not resolved yet */
static int patch_import(HMODULE mod, const char *dll, const char *func, void *hook, void **orig)
{
    BYTE *img = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)img;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(img + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d;

    if (!dir->VirtualAddress)
        return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(img + dir->VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *names, *slot;

        if (_stricmp((char *)(img + d->Name), dll))
            continue;
        names = (IMAGE_THUNK_DATA *)(img + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        slot = (IMAGE_THUNK_DATA *)(img + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slot++) {
            DWORD old;

            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)
                || strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(img + names->u1.AddressOfData))->Name, func))
                continue;
            if (d->OriginalFirstThunk && slot->u1.Function == names->u1.Function)
                return -1;      /* still the name's RVA: the loader has not got to it */
            if (!VirtualProtect(&slot->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
                return 0;
            if (orig && !*orig)
                *orig = (void *)slot->u1.Function;
            slot->u1.Function = (DWORD_PTR)hook;
            VirtualProtect(&slot->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

/* Try the patch; called again from the frame tick while it answers 1 (loader order) */
int redirect_retry(void)
{
    int r;

    if (installed || !base[0])
        return 0;
    r = patch_import(GetModuleHandleA(NULL), "shell32.dll", "SHGetSpecialFolderPathA", (void *)special_folder,
                     (void **)&real_special);
    if (r > 0) {
        installed = 1;
        logmsg("redirect: user data folder of the game is %s", base);
    } else if (r == 0) {
        /* fail closed: a run that asked for isolation must not write into the real profile */
        logmsg("redirect: the exe does not import SHGetSpecialFolderPathA: cannot redirect, exiting");
        diag_log_close();
        ExitProcess(125);
    }
    return r < 0;
}

/* The folder standing in for %LOCALAPPDATA%: the redirect if on, else the environment. 0: unknown */
int redirect_data_base(char *out, size_t n)
{
    DWORD len;

    if (base[0]) {
        snprintf(out, n, "%s", base);
        return (int)strlen(out);
    }
    len = GetEnvironmentVariableA("LOCALAPPDATA", out, (DWORD)n);
    return len && len < n ? (int)len : 0;
}

int redirect_active(void)
{
    return base[0] != 0;
}

void redirect_init(void)
{
    char ini[MAX_PATH], val[MAX_PATH];

    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    GetPrivateProfileStringA("test", "save_dir", "", val, sizeof(val), ini);
    base[0] = 0;
    if (!val[0])
        return;
    if (val[1] == ':')
        snprintf(base, MAX_PATH, "%s", val);
    else
        snprintf(base, MAX_PATH, "%s\\%s", game_dir, val);
    while (strlen(base) > 3 && base[strlen(base) - 1] == '\\')
        base[strlen(base) - 1] = 0;
    make_dirs(base);
    logmsg("redirect: [test] save_dir = %s", base);
    if (redirect_retry())
        logmsg("redirect: imports not resolved yet, trying again from the frame tick");
}
