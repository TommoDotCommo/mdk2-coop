/* Launcher adapter: UI owns profiles; worker snapshots under a short lock and
 * performs DNS and UDP outside it. Store replacement is atomic on Windows. */
#include <ws2tcpip.h>
#include "launcher_profiles.h"
static struct lp_store profiles;
static struct lp_probe profile_probes[LP_MAX];
static CRITICAL_SECTION profile_lock;
static LONG profile_stop;
static int profile_loaded, profile_bad, profile_generation, profile_selected, profile_top, profile_edit;
static struct lp_profile profile_draft;
static char profile_port[8], profile_message[180];
static int profile_read(const char *path, struct lp_store *store)
{
    char bytes[LP_BYTES];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(bytes, 1, sizeof(bytes), f);
    if (ferror(f)) { fclose(f); return 0; }
    fclose(f);
    return lp_decode(store, bytes, n);
}
static int profile_write(const char *path, const struct lp_store *store)
{
    char bytes[LP_BYTES], stage[MAX_PATH], backup[MAX_PATH];
    FILE *f;
    int n = lp_encode(store, bytes, sizeof(bytes)), ok;
    if (!n || strlen(path) + 10 >= MAX_PATH) return 0;
    snprintf(stage, sizeof(stage), "%s.tmp", path);
    snprintf(backup, sizeof(backup), "%s.previous", path);
    /* Preserve the previous bytes, including a corrupt store explicitly replaced
     * through import. Do not overwrite that recovery copy on later writes. */
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && GetFileAttributesA(backup) == INVALID_FILE_ATTRIBUTES &&
        !CopyFileA(path, backup, TRUE)) return 0;
    f = fopen(stage, "wb");
    if (!f) return 0;
    ok = fwrite(bytes, 1, n, f) == (size_t)n;
    if (fclose(f)) ok = 0;
    if (!ok || !MoveFileExA(stage, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(stage); return 0;
    }
    return 1;
}
static int profile_save(struct lp_store *next)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\profiles.tsv", coop_dir);
    if (!profile_write(path, next)) { strcpy(profile_message, "Could not save. Your existing profile file was kept."); return 0; }
    EnterCriticalSection(&profile_lock);
    profiles = *next; profile_generation++; memset(profile_probes, 0, sizeof(profile_probes));
    LeaveCriticalSection(&profile_lock);
    profile_bad = 0; strcpy(profile_message, "Saved locally. Export files contain your addresses.");
    return 1;
}
static DWORD WINAPI profile_worker(LPVOID arg)
{
    WSADATA wsa;
    struct lp_store snapshot;
    struct lp_probe results[LP_MAX] = {{0}};
    int generation = -1, index = 0;
    (void)arg;
    if (WSAStartup(MAKEWORD(2,2), &wsa)) return 0;
    while (!InterlockedCompareExchange(&profile_stop, 0, 0)) {
        int current;
        EnterCriticalSection(&profile_lock);
        current = profile_generation;
        if (current != generation) { snapshot = profiles; memset(results, 0, sizeof(results)); generation = current; index = 0; }
        LeaveCriticalSection(&profile_lock);
        if (snapshot.n && lp_due(&results[index], GetTickCount())) {
            struct addrinfo hints = {0}, *resolved = NULL;
            struct sockaddr_in endpoint;
            SOCKET sock = INVALID_SOCKET;
            unsigned char query[8], reply[128];
            struct lp_probe *probe = &results[index];
            uint32_t now = GetTickCount(), nonce;
            char service[8];
            hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; hints.ai_protocol = IPPROTO_UDP;
            snprintf(service, sizeof(service), "%u", snapshot.p[index].port);
            probe->address = 0;
            /* System DNS runs only on this worker, never on the UI thread. */
            if (!getaddrinfo(snapshot.p[index].host, service, &hints, &resolved) && resolved) {
                endpoint = *(struct sockaddr_in *)resolved->ai_addr;
                probe->address = endpoint.sin_addr.s_addr;
                sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            }
            if (resolved) freeaddrinfo(resolved);
            nonce = GetTickCount() ^ (DWORD)GetCurrentProcessId() ^ ((unsigned)rand() << 16);
            now = GetTickCount(); lp_sent(probe, now, nonce);
            if (sock != INVALID_SOCKET) {
                u_long nonblock = 1;
                ioctlsocket(sock, FIONBIO, &nonblock);
                memcpy(query, "CPQ1", 4);
                query[4] = nonce; query[5] = nonce>>8; query[6] = nonce>>16; query[7] = nonce>>24;
                sendto(sock, (const char *)query, 8, 0, (struct sockaddr *)&endpoint, sizeof(endpoint));
                while (!InterlockedCompareExchange(&profile_stop,0,0) && (DWORD)(GetTickCount()-now) < 1200 && probe->pending) {
                    struct sockaddr_in from;
                    int len = sizeof(from), n = recvfrom(sock, (char *)reply, sizeof(reply), 0, (struct sockaddr *)&from, &len);
                    if (n > 0) lp_reply(probe, GetTickCount(), reply, n,
                        from.sin_addr.s_addr == endpoint.sin_addr.s_addr && from.sin_port == endpoint.sin_port, MOD_VERSION);
                    Sleep(20);
                }
                closesocket(sock);
            }
            lp_due(probe, GetTickCount()+1);
            if (probe->pending) { probe->pending = 0; probe->status = LP_OFFLINE; }
            EnterCriticalSection(&profile_lock);
            if (profile_generation == generation) profile_probes[index] = *probe;
            LeaveCriticalSection(&profile_lock);
        }
        if (snapshot.n) index = (index+1) % snapshot.n;
        Sleep(50);
    }
    WSACleanup(); return 0;
}
static void profile_load(void)
{
    char path[MAX_PATH];
    HANDLE thread;
    if (profile_loaded) return;
    InitializeCriticalSection(&profile_lock); profile_loaded = 1;
    snprintf(path, sizeof(path), "%s\\profiles.tsv", coop_dir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && !profile_read(path, &profiles)) {
        profile_bad = 1; strcpy(profile_message, "Unreadable profiles kept. Import a valid export to recover.");
    }
    thread = CreateThread(NULL, 0, profile_worker, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    else strcpy(profile_message, "Background status unavailable. Profiles can still be used.");
}
/* Save a lobby endpoint without replacing an existing address's name or notes. */
static int profile_save_endpoint(const char *host, unsigned port)
{
    struct lp_store next;
    struct lp_profile entry = {0};
    int i, suffix;
    profile_load();
    if (profile_bad) return 0;
    next = profiles;
    for (i = 0; i < next.n; i++) {
        if (!_stricmp(next.p[i].host, host) && next.p[i].port == port) {
            profile_selected = i;
            strcpy(profile_message, "This address is already saved.");
            return 1;
        }
    }
    if (next.n == LP_MAX) {
        strcpy(profile_message, "Saved addresses are full. Remove an address before adding another.");
        return 0;
    }
    snprintf(entry.host, sizeof(entry.host), "%s", host);
    entry.port = port;
    for (suffix = 0; suffix <= LP_MAX; suffix++) {
        if (suffix) snprintf(entry.name, sizeof(entry.name), "%.30s:%u (%d)", host, port, suffix);
        else snprintf(entry.name, sizeof(entry.name), "%.40s:%u", host, port);
        for (i = 0; i < next.n; i++) if (!strcmp(next.p[i].name, entry.name)) break;
        if (i == next.n) break;
    }
    if (!lp_valid(&entry)) {
        strcpy(profile_message, "This lobby does not have an address that can be saved.");
        return 0;
    }
    next.p[next.n++] = entry;
    if (!profile_save(&next)) return 0;
    profile_selected = next.n - 1;
    strcpy(profile_message, "Address saved. Find it under Saved addresses.");
    return 1;
}
static int profile_session_active(void);
static void profile_join(int host)
{
    struct lp_store next = profiles;
    struct lp_profile *p;
    uint32_t address;
    struct in_addr a;
    if (profile_selected < 0 || profile_selected >= next.n) return;
    if (profile_session_active()) { strcpy(profile_message, "Leave the current lobby before joining another profile."); return; }
    p = &next.p[profile_selected];
    EnterCriticalSection(&profile_lock); address = profile_probes[profile_selected].address; LeaveCriticalSection(&profile_lock);
    /* Host names resolve only on the worker; numeric profiles can be used offline. */
    if (!address) address = inet_addr(p->host);
    if (!host && (!address || address == INADDR_NONE)) { strcpy(profile_message, "Waiting for host name resolution. Try again after the status check."); return; }
    p->used = (uint64_t)time(NULL);
    if (!profile_save(&next)) return;
    cfg.steam = 0; cfg.mode = host ? MODE_HOST : MODE_JOIN;
    snprintf(cfg.port, sizeof(cfg.port), "%u", p->port);
    if (host) snprintf(cfg.listen, sizeof(cfg.listen), "%u", p->port);
    a.s_addr = address;
    snprintf(cfg.host, sizeof(cfg.host), "%s", inet_ntoa(a));
    profile_edit = 0; page = PAGE_PLAY; changed = 1;
    lob_open();
}
static void page_profiles(void)
{
    struct lp_store next;
    int i;
    float y = 150;
    char line[200];
    profile_load();
    heading(PX, 96, 22, "Saved addresses", C_BLUE, DT_LEFT);
    if (link(ID, PX+PW-42, 100, "Back")) { profile_edit = 0; page = PAGE_PLAY; changed = 1; }
    if (profile_edit) {
        field(ID, PX, y, PW, "Name", profile_draft.name, sizeof(profile_draft.name), 0, "Friends"); y += 72;
        field(ID, PX, y, PW-110, "Host or IPv4 address", profile_draft.host, sizeof(profile_draft.host), 0, "127.0.0.1");
        field(ID, PX+PW-100, y, 100, "Port", profile_port, sizeof(profile_port), 1, "27900"); y += 72;
        field(ID, PX, y, PW, "Notes", profile_draft.notes, sizeof(profile_draft.notes), 0, "Private notes"); y += 80;
        if (button(ID, PX, y, 145, 42, "Save", 1) && !profile_bad) {
            uint64_t port;
            next = profiles;
            if (lp_number(profile_port, &port) && port <= 65535) profile_draft.port = (unsigned)port;
            else profile_draft.port = 0;
            i = profile_selected;
            if (i == next.n && next.n < LP_MAX) next.n++;
            if (i >= 0 && i < next.n) {
                char check[LP_BYTES];
                next.p[i] = profile_draft;
                if (lp_encode(&next, check, sizeof(check)) && profile_save(&next)) profile_edit = 0;
                else strcpy(profile_message, "Use a unique name, valid host and port (1 to 65535).");
            }
            changed = 1;
        }
        if (button(ID, PX+160, y, 145, 42, "Cancel", 0)) { profile_edit = 0; changed = 1; }
        if (profile_selected < profiles.n && button(ID, PX+320, y, 145, 42, "Delete", 0)) {
            next = profiles;
            memmove(&next.p[profile_selected], &next.p[profile_selected+1], (next.n-profile_selected-1)*sizeof(next.p[0])); next.n--;
            if (profile_save(&next)) { profile_edit = 0; profile_selected = 0; profile_top = 0; }
            changed = 1;
        }
    } else {
        static const char *status[] = { "Checking...", "No answer", "Reachable", "In lobby", "In game", "Version mismatch", "Full" };
        for (i = profile_top; i < profiles.n && i < profile_top+4; i++) {
            struct lp_probe probe;
            char when[32];
            EnterCriticalSection(&profile_lock); probe = profile_probes[i]; LeaveCriticalSection(&profile_lock);
            ago((long)profiles.p[i].used, when, sizeof(when));
            if (probe.status >= LP_REACHABLE) {
                snprintf(line, sizeof(line), "%s:%u  -  %s (%s, %d ms)  -  %s%s%s", profiles.p[i].host, profiles.p[i].port,
                         status[probe.status], probe.version, probe.rtt, when, profiles.p[i].notes[0] ? "  -  " : "", profiles.p[i].notes);
            } else {
                snprintf(line, sizeof(line), "%s:%u  -  %s  -  %s%s%s", profiles.p[i].host, profiles.p[i].port,
                         status[probe.status], when, profiles.p[i].notes[0] ? "  -  " : "", profiles.p[i].notes);
            }
            clip_text(line, PW - 36);
            if (tile(ID+7000+i, PX, y, PW, 66, profiles.p[i].name, line, profile_selected == i)) { profile_selected = i; changed = 1; }
            y += 72;
        }
        y = 446;
        if (link(ID, PX, y, "Previous") && profile_top > 0) { profile_top -= 4; changed = 1; }
        if (link(ID, PX+110, y, "Next") && profile_top+4 < profiles.n) { profile_top += 4; changed = 1; }
        if (link(ID, PX+210, y, "Add") && profiles.n < LP_MAX && !profile_bad) {
            memset(&profile_draft, 0, sizeof(profile_draft)); strcpy(profile_port, "27900"); profile_selected = profiles.n; profile_edit = 1; changed = 1;
        }
        if (link(ID, PX+280, y, "Edit") && profile_selected < profiles.n) {
            profile_draft = profiles.p[profile_selected]; snprintf(profile_port, sizeof(profile_port), "%u", profile_draft.port); profile_edit = 1; changed = 1;
        }
        y += 30;
        if (button(ID, PX, y, 100, 38, "Join", 1) && profile_selected < profiles.n) profile_join(0);
        if (button(ID, PX+110, y, 100, 38, "Host", 0) && profile_selected < profiles.n) profile_join(1);
        if (button(ID, PX+220, y, 115, 38, "Export", 0)) {
            char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\profiles-export.tsv", coop_dir);
            strcpy(profile_message, profile_write(path, &profiles) ? "Exported profiles-export.tsv in the mod folder." : "Export failed; existing export was kept."); changed = 1;
        }
        if (button(ID, PX+345, y, 115, 38, "Import", 0)) {
            char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\profiles-import.tsv", coop_dir);
            if (profile_read(path, &next) && profile_save(&next)) { profile_selected = profile_top = 0; }
            else strcpy(profile_message, "Import rejected; profiles-import.tsv must be a valid export.");
            changed = 1;
        }
    }
    gdi_para(f_small, PX, 526, PW, profile_bad ? C_BAD : C_DIM, profile_message);
}
