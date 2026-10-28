/* Game-free Windows fixture for the production WinINet adapter and mapped-DLL
 * replacement. Run from a disposable Windows folder with an isolated profile. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>
#include "launcher_profiles.h"
#define MOD_VERSION "v0.3.2-test"
static char game_dir[MAX_PATH], ini[MAX_PATH];
static int changed, test_background = 1;
static void put(const char *s, const char *k, const char *v) { assert(WritePrivateProfileStringA(s,k,v,ini)); }
static void put_int(const char *s, const char *k, int v) { char b[32]; snprintf(b,sizeof(b),"%d",v); put(s,k,b); }
#include "launcher_update_win.h"
static void wait_worker(void)
{
    DWORD start = GetTickCount();
    while (InterlockedCompareExchange(&update_busy,0,0)) {
        assert(GetTickCount() - start < 30000);
        Sleep(10);
    }
}
int main(int argc, char **argv)
{
    HMODULE mapped;
    char dll[MAX_PATH], lua[MAX_PATH], before[65], after[65], before_lua[65];
    int mode;
    assert(argc == 4);
    snprintf(game_dir,sizeof(game_dir),"%s",argv[1]);
    snprintf(ini,sizeof(ini),"%s/coop/coop.ini",game_dir);
    if (!strcmp(argv[2], "real-api")) {
        int status, remaining; uint32_t retry; unsigned char *body; size_t length;
        int ok=update_wininet_get(DEFAULT_RELEASE_API,&status,&remaining,&retry,&body,&length,LU_MAX_JSON_BYTES);
        printf("Real GitHub API read-only attempt: transport=%d HTTP=%d bytes=%u remaining=%d\n",ok,status,(unsigned)length,remaining);
        free(body); return 0;
    }
    mode=atoi(argv[3]);
    put("test","background","1"); put("update","api_url",argv[2]);
    snprintf(dll,sizeof(dll),"%s/dinput8.dll",game_dir);
    snprintf(lua,sizeof(lua),"%s/coop/coop.lua",game_dir);
    assert(lu_file_sha256(lua,before_lua));
    assert(lu_file_sha256(dll,before));
    mapped=LoadLibraryA(dll); assert(mapped);
    update_init(); wait_worker();
    printf("check state=%d error=%d message=%s\n",update_state,update_last_err,update_msg); fflush(stdout);
    if (mode == 1) assert(update_state == UP_CURRENT);
    else if (mode == LU_ERR_ASSET) assert(update_state == UP_ERROR && update_last_err == mode);
    else if (mode == LU_ERR_IO) {
        int status, remaining; uint32_t retry; unsigned char *body; size_t length;
        assert(update_state == UP_AVAILABLE);
        assert(update_wininet_get(update_rel.zip_url,&status,&remaining,&retry,&body,&length,LU_MAX_ASSET_BYTES));
        assert(status == 200);
        assert(lu_apply_package(game_dir,body,length,"mdk2-coop",MOD_VERSION,update_rel.tag,1) == LU_ERR_IO);
        free(body);
        puts("PASS: injected failure after mapped DLL replacement rolls back both files");
    } else {
        assert(update_state == UP_AVAILABLE);
        update_start_apply(); wait_worker();
        if (mode) assert(update_state == UP_ERROR && update_last_err == mode);
        else {
            assert(update_state == UP_APPLIED && update_last_err == LU_OK && update_has_rollback());
            assert(lu_file_sha256(dll,after) && strcmp(before,after));
            FreeLibrary(mapped); mapped=LoadLibraryA(dll); assert(mapped);
            FreeLibrary(mapped); mapped=NULL;
            update_do_rollback();
            assert(update_state == UP_ROLLED_BACK && update_last_err == LU_OK);
            puts("PASS: real bridge replaced while mapped, replacement loaded, verified rollback");
        }
    }
    assert(lu_file_sha256(dll,after) && !strcmp(before,after));
    assert(lu_file_sha256(lua,after) && !strcmp(before_lua,after));
    if (mapped) FreeLibrary(mapped);
    printf("PASS: Windows update scenario %d; original DLL hash preserved/restored\n",mode);
    return 0;
}
