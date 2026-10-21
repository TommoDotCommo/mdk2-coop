/* Opt-in integration smoke test. Uses the player's Steam account to create and
 * leave a friends-only lobby under MDK2 HD's AppID. Never sends invites. Run only
 * with permission, using a Windows scratch directory and the game's original DLL.
 *   steam_smoke.exe <absolute steam_api.dll path> <scratch directory>
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "../bridge/bridge.h"
#include "../bridge/steam.c"

char coop_dir[MAX_PATH];
int role_host = 1, cur_level = 0;
static int session_resets;
void net_steam_session(int host, uint64_t peer) { role_host = host; session_resets++; }
void net_steam_receive(const void *data, int len, uint64_t peer) { }
void net_steam_left(uint64_t peer) { }
double now_ms(void)
{
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}
void logmsg(const char *fmt, ...)
{
    va_list args; va_start(args,fmt); vprintf(fmt,args); va_end(args); puts(""); fflush(stdout);
}
int main(int argc, char **argv)
{
    HMODULE api;
    bool (__cdecl *init)(void);
    void (__cdecl *shutdown)(void);
    void (__cdecl *unregister_cb)(struct steam_callback *);
    char ini[MAX_PATH];
    int round, i, result=1;
    double deadline;
    if (argc != 3) { puts("Usage: steam_smoke.exe <original Steam DLL> <scratch directory>"); return 2; }
    snprintf(coop_dir,sizeof(coop_dir),"%s",argv[2]);
    snprintf(ini,sizeof(ini),"%s\\steam-smoke.ini",coop_dir);
    SetEnvironmentVariableA("SteamAppId","213350");
    SetEnvironmentVariableA("SteamGameId","213350");
    api=LoadLibraryExA(argv[1],NULL,LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!api) { printf("FAIL: load original Steam DLL (%lu)\n",GetLastError()); return 1; }
    init=(void *)GetProcAddress(api,"SteamAPI_Init");
    shutdown=(void *)GetProcAddress(api,"SteamAPI_Shutdown");
    unregister_cb=(void *)GetProcAddress(api,"SteamAPI_UnregisterCallback");
    if (!init || !shutdown || !unregister_cb || !init()) { puts("FAIL: original Steam API could not initialize"); FreeLibrary(api); return 1; }
    steam_open(ini);
    for (round=0;round<2;round++) {
        if (round && !steam_host_lobby()) { printf("FAIL: second lobby request: %s\n",steam_status()); goto cleanup; }
        deadline=now_ms()+40000;
        while (!steam_ready() && now_ms()<deadline) { steam_poll(); Sleep(25); }
        if (!steam_ready()) { printf("FAIL: lobby did not become ready: %s\n",steam_status()); goto cleanup; }
        if (!role_host || member_count!=1 || host_id_steam!=self_id) { puts("FAIL: unexpected lobby membership or owner"); goto cleanup; }
        if (strcmp(SMETHOD(matchmaking,MM_DATA,const char *,uint64_t,const char *)(matchmaking,lobby_id,PROTOCOL_KEY),PROTOCOL_VALUE)) {
            puts("FAIL: lobby protocol metadata did not round trip"); goto cleanup;
        }
        printf("PASS: original Steam API, versioned interfaces, lobby creation, owner/member ABI, metadata (round %d)\n",round+1);
        if (!steam_leave_lobby() || steam_ready() || lobby_text[0]) { puts("FAIL: lobby cleanup"); goto cleanup; }
        for (i=0;i<20;i++) { steam_poll(); Sleep(25); }
    }
    puts("PASS: real Steam lobby lifecycle and cleanup; no invites sent. Two-account relay/gameplay is not covered.");
    result=0;
cleanup:
    if (lobby_id) leave();
    for (i=0;initialized && i<4;i++) unregister_cb(&callbacks[i]);
    shutdown();
    FreeLibrary(api);
    return result;
}
