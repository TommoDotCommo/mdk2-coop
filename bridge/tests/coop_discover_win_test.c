/* Windows self-test of coop_discover_win.h, game-free.
 * Default: lists the up IPv4 adapters and the broadcast candidates built from them, runs
 * `tailscale status --json` (counts only, no names printed), and probes fake hosts this program
 * starts on 127.0.0.1-127.0.0.4 (right game, a second address, wrong game, invite only)
 * and 127.0.0.5, which answers from another address.
 * --adapters: also binds a fake host on every adapter address and scans by directed broadcast.
 * Binding a non-loopback address can make Windows Firewall ask about this program, so that
 * mode is for a person at the desk. Exit 0 = every check passed. */
#include "coop_discover_win.h"
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
#define MAX_HOSTS 24

struct fake { SOCKET s, via; struct cd_host h; uint32_t ip; unsigned port; int answered; };
static struct fake hosts[MAX_HOSTS];
static int nhosts;
static volatile LONG stop_hosts;
static struct cd_rate rate;

static DWORD WINAPI host_thread(LPVOID arg)
{
    (void)arg;
    while (!InterlockedCompareExchange(&stop_hosts, 0, 0)) {
        fd_set ready;
        struct timeval wait = { 0, 20000 };
        int i;
        FD_ZERO(&ready);
        for (i = 0; i < nhosts; i++) FD_SET(hosts[i].s, &ready);
        if (select(0, &ready, NULL, NULL, &wait) <= 0) continue;
        for (i = 0; i < nhosts; i++) {
            uint8_t q[64], reply[CD_REPLY_MAX];
            struct sockaddr_in from;
            int len = sizeof(from), n, m;
            if (!FD_ISSET(hosts[i].s, &ready)) continue;
            n = recvfrom(hosts[i].s, (char *)q, sizeof(q), 0, (struct sockaddr *)&from, &len);
            if (n <= 0 || !cd_rate_allow(&rate, ntohl(from.sin_addr.s_addr), GetTickCount())) continue;
            m = cd_answer(q, (size_t)n, &hosts[i].h, reply);
            if (m > 0 && sendto(hosts[i].via ? hosts[i].via : hosts[i].s, (const char *)reply, m, 0, (struct sockaddr *)&from, len) == m)
                hosts[i].answered++;
        }
    }
    return 0;
}
static int add_host(uint32_t ip, unsigned port, const char *game, const char *name, int invite_only)
{
    struct sockaddr_in a;
    struct fake *f = &hosts[nhosts];
    memset(f, 0, sizeof(*f));
    f->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ip); a.sin_port = htons((u_short)port);
    if (f->s == INVALID_SOCKET || bind(f->s, (struct sockaddr *)&a, sizeof(a))) {
        printf("could not bind a fake host (error %d)\n", WSAGetLastError());
        if (f->s != INVALID_SOCKET) closesocket(f->s);
        return 0;
    }
    if (!port) { int len = sizeof(a); getsockname(f->s, (struct sockaddr *)&a, &len); port = ntohs(a.sin_port); }
    f->ip = ip; f->port = port;
    f->h.game_id = game; f->h.version = "v-test"; f->h.name = name; f->h.state = CD_IN_LOBBY;
    f->h.invite_only = invite_only; f->h.players = 1; f->h.limit = 4;
    nhosts++;
    return (int)port;
}
struct collect { struct cd_list list; const struct cd_candidates *c; struct cd_he *he; int replies; };
static void on_reply(void *ctx, int at, uint32_t ip, unsigned port, const struct cd_reply *r, uint32_t rtt)
{
    struct collect *k = (struct collect *)ctx;
    k->replies++;
    cd_seen(&k->list, r, k->c->c[at].route, ip, port, k->c->c[at].group, (int)rtt, GetTickCount(), "v-test");
    if (k->he) cd_he_reply(k->he, at, GetTickCount());
}
static const char *kind(uint32_t ip)
{
    if ((ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8) return "private";
    if (cd_tailscale_ip(ip)) return "100.64/10 (Tailscale or CGNAT)";
    if ((ip >> 16) == 0xa9fe) return "link-local";
    return "public";
}
static int row_named(const struct cd_list *l, const char *name)
{
    int i;
    for (i = 0; i < l->n; i++) if (!strcmp(l->r[i].name, name)) return i;
    return -1;
}

int main(int argc, char **argv)
{
    static struct cdw_adapter wa[32];
    static struct cd_adapter ad[32];
    static struct cd_candidates c;
    static struct cd_ts ts;
    static char status[1 << 20];
    static struct collect k;
    struct cd_he he;
    WSADATA wsa;
    HANDLE thread;
    int nad, i, j, port, sent, len, adapters_mode = argc > 1 && !strcmp(argv[1], "--adapters");
    DWORD t0;

    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { puts("FAIL: WSAStartup"); return 1; }
    /* --tailscale-expect missing|timeout|fixture: only the tailscale.exe step, against a stand-in */
    if (argc > 2 && !strcmp(argv[1], "--tailscale-expect")) {
        t0 = GetTickCount();
        len = cdw_tailscale_status(status, sizeof(status), 2000);
        printf("tailscale stand-in: result %d after %lu ms\n", len, (unsigned long)(GetTickCount() - t0));
        if (!strcmp(argv[2], "missing")) CHECK(len == 0);      /* also run from a folder holding a tailscale.exe */
        else if (!strcmp(argv[2], "timeout")) CHECK(len == -1 && GetTickCount() - t0 >= 1900 && GetTickCount() - t0 < 4000);
        else {
            CHECK(len > 0 && cd_ts_parse(status, (size_t)len, &ts) && ts.running && ts.total == 4 && ts.n == 2 && ts.offline == 1 && ts.no_ipv4 == 1);
            CHECK(ts.peer[1].sharee && ts.self.ip == 0x64400001u && !strcmp(ts.self.dns, "me.tail0.ts.net"));
        }
        if (failures) { printf("FAILED: %d checks\n", failures); return 1; }
        printf("PASS: tailscale %s\n", argv[2]);
        return 0;
    }
    /* 1. adapters */
    nad = cdw_adapters(wa, 32);
    CHECK(nad >= 0);
    printf("adapters: %d up IPv4 address(es)\n", nad);
    for (i = 0; i < nad; i++)
        printf("  [%d] %-28.28s type %3lu  /%-2d  %s\n", i, wa[i].name, wa[i].type, wa[i].a.prefix, kind(wa[i].a.ip));
    cdw_adapter_list(wa, nad, ad);
    memset(&c, 0, sizeof(c));
    printf("broadcast candidates: %d (default port and the next three per subnet)\n", cd_add_adapters(&c, ad, nad, 27900));
    for (i = 0; i < c.n; i++) CHECK(c.c[i].ip != 0xffffffffu && c.c[i].broadcast);

    /* 2. Tailscale */
    t0 = GetTickCount();
    len = cdw_tailscale_status(status, sizeof(status), 2000);
    if (!len) puts("tailscale: not installed (PATH, %ProgramW6432%, %ProgramFiles%)");
    else if (len < 0) printf("tailscale: found, but status failed or timed out after %lu ms\n", (unsigned long)(GetTickCount() - t0));
    else {
        int parsed = cd_ts_parse(status, (size_t)len, &ts);
        printf("tailscale: found; status --json %d bytes in %lu ms; parsed %s; backend %s; %d peers: %d online with IPv4, %d offline, %d online without IPv4; this PC has %s\n",
               len, (unsigned long)(GetTickCount() - t0), parsed ? "yes" : "NO", ts.running ? "running" : "not running",
               ts.total, ts.n, ts.offline, ts.no_ipv4, ts.self.ip ? "an IPv4 Tailscale address" : "no IPv4 Tailscale address");
        CHECK(parsed);
        CHECK(GetTickCount() - t0 < 4000);
        memset(&c, 0, sizeof(c));
        printf("tailscale candidates: %d\n", cd_add_tailscale(&c, &ts, 27900));
    }
    memset(status, 0, sizeof(status));      /* nothing read is kept */

    /* 3. fake hosts on loopback: Fox on 127.0.0.1:P, Owl on 127.0.0.2:P+2, a wrong-game host on
     * 127.0.0.3:P and an invite-only host on 127.0.0.4:P+1 */
    port = add_host(0x7f000001u, 0, "MDK2", "Fox", 0);
    CHECK(port > 0 && port < 65530);
    if (port <= 0) return 1;
    CHECK(add_host(0x7f000002u, (unsigned)port + 2, "MDK2", "Owl", 0) > 0);
    CHECK(add_host(0x7f000003u, (unsigned)port, "WIT ", "Other game", 0) > 0);
    CHECK(add_host(0x7f000004u, (unsigned)port + 1, "MDK2", "Secret", 1) > 0);
    /* a host that answers from another address than the one asked (spoofed or misrouted) */
    CHECK(add_host(0x7f000005u, (unsigned)port + 3, "MDK2", "Rogue", 0) > 0);
    {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x7f000006u); a.sin_port = htons((u_short)(port + 3));
        hosts[4].via = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(hosts[4].via != INVALID_SOCKET && !bind(hosts[4].via, (struct sockaddr *)&a, sizeof(a)));
    }
    thread = CreateThread(NULL, 0, host_thread, NULL, 0, NULL);
    CHECK(thread != NULL);
    memset(&c, 0, sizeof(c));
    for (i = 1; i <= 5; i++) for (j = 0; j < CD_PORTS; j++) cd_add(&c, 0x7f000000u | (uint32_t)i, (unsigned)port + (unsigned)j, CD_LAN, 0, 0, -1);
    memset(&k, 0, sizeof(k)); k.c = &c;
    t0 = GetTickCount();
    sent = cdw_scan(&c, 0x5eed1234u, "MDK2", 600, 0x7f000001u, on_reply, &k, NULL);
    printf("loopback scan: %d queries, %d replies, %d rows in %lu ms\n", sent, k.replies, k.list.n, (unsigned long)(GetTickCount() - t0));
    CHECK(sent == 20);
    CHECK(k.list.n == 2 && row_named(&k.list, "Fox") >= 0 && row_named(&k.list, "Owl") >= 0);
    CHECK(row_named(&k.list, "Other game") < 0 && row_named(&k.list, "Secret") < 0 && row_named(&k.list, "Rogue") < 0);
    if (row_named(&k.list, "Owl") >= 0) CHECK(k.list.r[row_named(&k.list, "Owl")].port == (unsigned)port + 2 && k.list.r[row_named(&k.list, "Owl")].rtt < 600);
    CHECK(hosts[2].answered == 0 && hosts[3].answered == 1 && hosts[4].answered == 1);   /* wrong game silent; invite-only answers without its name */
    /* a reply with another nonce is ignored: scan again expecting a different game id */
    memset(&k, 0, sizeof(k)); k.c = &c;
    cdw_scan(&c, 0x5eed1235u, "WIT ", 300, 0x7f000001u, on_reply, &k, NULL);
    CHECK(k.list.n == 1 && row_named(&k.list, "Other game") == 0);
    /* the invite-only host is listed for its invite, with no name */
    memset(&c, 0, sizeof(c)); cd_add(&c, 0x7f000004u, (unsigned)port + 1, CD_INTERNET, 0, 0, 1);
    memset(&k, 0, sizeof(k)); k.c = &c;
    cdw_scan(&c, 0x5eed1236u, "MDK2", 300, 0x7f000001u, on_reply, &k, NULL);
    CHECK(k.list.n == 1 && !k.list.r[0].name[0] && (k.list.r[0].flags & CD_F_INVITE_ONLY));
    /* C4 over sockets: an invite with an internet route and a LAN route; both answer, LAN wins */
    {
        uint8_t routes[2] = { CD_INTERNET, CD_LAN };
        memset(&c, 0, sizeof(c));
        cd_add(&c, 0x7f000001u, (unsigned)port, CD_INTERNET, 0, 0, 2);
        cd_add(&c, 0x7f000002u, (unsigned)port + 2, CD_LAN, 0, 0, 2);
        cd_he_start(&he, routes, 2);
        memset(&k, 0, sizeof(k)); k.c = &c; k.he = &he;
        cdw_scan(&c, 0x5eed1237u, "MDK2", 400, 0x7f000001u, on_reply, &k, NULL);
        CHECK(cd_he_choice(&he, GetTickCount()) == 1);
    }
    /* rate limit over sockets: 40 queries at once from one source get at most about 10 answers */
    {
        int before = hosts[0].answered;
        memset(&c, 0, sizeof(c));
        cd_add(&c, 0x7f000001u, (unsigned)port, CD_LAN, 0, 0, -1);
        for (i = 1; i < 40; i++) c.c[i] = c.c[0];        /* cd_add de-duplicates: repeat it by hand */
        c.n = 40;
        memset(&rate, 0, sizeof(rate));
        memset(&k, 0, sizeof(k)); k.c = &c;
        cdw_scan(&c, 0x5eed1238u, "MDK2", 300, 0x7f000001u, on_reply, &k, NULL);
        printf("rate limit: 40 queries from one source, %d answered\n", hosts[0].answered - before);
        CHECK(hosts[0].answered - before >= 10 && hosts[0].answered - before <= 13);
    }

    /* 4. optional: a fake host on every adapter address, found by directed broadcast */
    if (adapters_mode) {
        int first = nhosts, found = 0;
        InterlockedExchange(&stop_hosts, 1); WaitForSingleObject(thread, 2000); CloseHandle(thread);
        InterlockedExchange(&stop_hosts, 0);
        static char names[MAX_HOSTS][16];
        for (i = 0; i < nad && nhosts < MAX_HOSTS; i++) {
            snprintf(names[i], sizeof(names[i]), "Adapter %d", i);
            add_host(wa[i].a.ip, (unsigned)port + 3, "MDK2", names[i], 0);
        }
        thread = CreateThread(NULL, 0, host_thread, NULL, 0, NULL);
        memset(&c, 0, sizeof(c)); cd_add_adapters(&c, ad, nad, (unsigned)port);
        memset(&k, 0, sizeof(k)); k.c = &c; memset(&rate, 0, sizeof(rate));
        sent = cdw_scan(&c, 0x5eed1239u, "MDK2", 1200, 0, on_reply, &k, NULL);
        for (i = first; i < nhosts; i++) {
            char name[16];
            int r;
            snprintf(name, sizeof(name), "Adapter %d", i - first);
            r = row_named(&k.list, name);
            printf("  adapter [%d] %-28.28s %s\n", i - first, wa[i - first].name,
                   r >= 0 ? "found by its subnet broadcast" : c.n ? "not found (no broadcast for this subnet, or not delivered)" : "-");
            found += r >= 0;
        }
        printf("adapter scan: %d broadcast queries, %d of %d adapter hosts found\n", sent, found, nhosts - first);
    }
    InterlockedExchange(&stop_hosts, 1);
    if (thread) { WaitForSingleObject(thread, 2000); CloseHandle(thread); }
    for (i = 0; i < nhosts; i++) { closesocket(hosts[i].s); if (hosts[i].via) closesocket(hosts[i].via); }
    WSACleanup();
    if (failures) { printf("FAILED: %d checks\n", failures); return 1; }
    puts("PASS: Windows adapters, Tailscale status, loopback fake hosts, nonce/game/invite-only rules, happy eyeballs, rate limit");
    return 0;
}
