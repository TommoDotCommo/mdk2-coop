/* The user data redirect against this program's own import of SHGetSpecialFolderPathA. No game needed. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../bridge.h"
#include <shlobj.h>

char coop_dir[MAX_PATH], game_dir[MAX_PATH];

void logmsg(const char *fmt, ...)
{
    (void)fmt;
}
void diag_log_close(void) {}

#include "../redirect.c"

/* one call per function: the compiler would otherwise load the import once and keep it in a register */
static __attribute__((noinline)) BOOL special(char *out, int csidl, BOOL create)
{
    return SHGetSpecialFolderPathA(NULL, out, csidl, create);
}

static void ini(const char *dir, const char *text)
{
    char path[MAX_PATH];
    FILE *f;

    snprintf(path, sizeof(path), "%s\\coop.ini", dir);
    f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    char root[MAX_PATH], want[MAX_PATH], got[MAX_PATH], other[MAX_PATH];

    (void)argc; (void)argv;
    GetTempPathA(sizeof(root), root);
    snprintf(root + strlen(root), sizeof(root) - strlen(root), "redirect-test-%lu", GetCurrentProcessId());
    snprintf(game_dir, sizeof(game_dir), "%s", root);
    snprintf(coop_dir, sizeof(coop_dir), "%s\\coop", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(coop_dir, NULL);

    /* off: nothing is hooked and the environment answers */
    ini(coop_dir, "[test]\r\nbackground=1\r\n");
    redirect_init();
    assert(!redirect_active());
    assert(special(got, CSIDL_LOCAL_APPDATA, FALSE));
    special(other, CSIDL_LOCAL_APPDATA, FALSE);
    assert(!strstr(got, "inst"));
    assert(redirect_data_base(want, sizeof(want)) > 0);

    /* on, relative to the game folder: the local app data answer moves, other folders do not */
    ini(coop_dir, "[test]\r\nsave_dir=inst\\profile\r\n");
    redirect_init();
    assert(redirect_active());
    assert(!redirect_retry());
    snprintf(want, sizeof(want), "%s\\inst\\profile", root);
    memset(got, 0, sizeof(got));
    assert(special(got, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, TRUE));
    assert(!strcmp(got, want));
    assert(GetFileAttributesA(want) != INVALID_FILE_ATTRIBUTES);
    assert(special(got, CSIDL_WINDOWS, FALSE));
    assert(strcmp(got, want) != 0);
    redirect_data_base(got, sizeof(got));
    assert(!strcmp(got, want));
    puts("redirect: ok");
    return 0;
}
