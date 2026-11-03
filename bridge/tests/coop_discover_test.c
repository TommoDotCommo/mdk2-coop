/* T0: probe v2, candidates, row merge, refresh, happy eyeballs, Tailscale status parser and
 * the recents store, with fuzzing. Usage: coop_discover_test [fuzz iterations] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "coop_discover.h"
#include "launcher_profiles.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static uint32_t seed = 0x9e3779b9u;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
#define IP(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static const char ts_fixture[] =
    "{\n  \"Version\": \"1.76.1\", \"TUN\": true, \"BackendState\": \"Running\",\n"
    "  \"AuthURL\": \"\", \"TailscaleIPs\": [\"100.101.1.1\", \"fd7a:115c:a1e0::1\"],\n"
    "  \"Self\": {\"ID\": \"n1\", \"HostName\": \"fox-pc\", \"DNSName\": \"fox-pc.tail1234.ts.net.\", \"OS\": \"windows\",\n"
    "           \"TailscaleIPs\": [\"100.101.1.1\", \"fd7a:115c:a1e0::1\"], \"Online\": true, \"Caps\": [\"a\", {\"b\": [1, 2.5e3, -4]}]},\n"
    "  \"Health\": [\"some \\\"warning\\\" \\u00e9\"], \"MagicDNSSuffix\": \"tail1234.ts.net\",\n"
    "  \"Peer\": {\n"
    "    \"nodekey:aa\": {\"HostName\": \"owl-laptop\", \"DNSName\": \"owl-laptop.tail1234.ts.net.\", \"OS\": \"windows\",\n"
    "       \"TailscaleIPs\": [\"fd7a:115c:a1e0::2\", \"100.101.1.2\"], \"Online\": true, \"Relay\": \"syd\", \"RxBytes\": 123,\n"
    "       \"LastSeen\": \"0001-01-01T00:00:00Z\", \"PrimaryRoutes\": null, \"Tags\": [], \"Unknown\": {\"x\": {\"y\": [[], {}]}}},\n"
    "    \"nodekey:bb\": {\"HostName\": \"badger\", \"DNSName\": \"badger.tail1234.ts.net.\", \"OS\": \"linux\",\n"
    "       \"TailscaleIPs\": [\"100.101.1.3\"], \"Online\": false},\n"
    "    \"nodekey:cc\": {\"HostName\": \"v6-only\", \"DNSName\": \"v6-only.tail1234.ts.net.\", \"OS\": \"linux\",\n"
    "       \"TailscaleIPs\": [\"fd7a:115c:a1e0::4\"], \"Online\": true},\n"
    "    \"nodekey:dd\": {\"HostName\": \"Zo\\u00eb's PC \\ud83e\\udd8a\", \"DNSName\": \"zoe-pc.other-tailnet.ts.net.\", \"OS\": \"windows\",\n"
    "       \"TailscaleIPs\": [\"100.88.7.6\"], \"Online\": true, \"ShareeNode\": true},\n"
    "    \"nodekey:ee\": {\"HostName\": \"no-ips\", \"TailscaleIPs\": null, \"Online\": true}\n"
    "  },\n  \"User\": {\"1\": {\"ID\": 1, \"LoginName\": \"x\"}}, \"ClientVersion\": null\n}\n";

static void reply_for(const struct cd_host *h, uint32_t nonce, uint8_t *reply, int *n)
{
    uint8_t q[CD_QUERY_LEN];
    cd_query(q, nonce, h->game_id, 0);
    *n = cd_answer(q, sizeof(q), h, reply);
}

/* A model of one PC with several adapters. Windows sends 255.255.255.255 out of one adapter only
 * (the first here); a directed broadcast reaches the subnet of the adapter that owns it. */
struct sim_host { uint32_t ip; int prefix; unsigned port; const char *name; };
static int sim_scan(const struct cd_adapter *ad, int nad, const struct sim_host *hosts, int nh, unsigned port, struct cd_list *list)
{
    struct cd_candidates c;
    int i, j, found = 0;
    memset(&c, 0, sizeof(c)); memset(list, 0, sizeof(*list));
    cd_add_adapters(&c, ad, nad, port);
    for (i = 0; i < c.n; i++) {
        for (j = 0; j < nh; j++) {
            uint32_t mask = cd_mask(hosts[j].prefix), bcast = hosts[j].ip | ~mask;
            int reach = c.c[i].port == hosts[j].port &&
                        (c.c[i].ip == hosts[j].ip || c.c[i].ip == bcast ||
                         (c.c[i].ip == 0xffffffffu && (hosts[j].ip & cd_mask(ad[0].prefix)) == (ad[0].ip & cd_mask(ad[0].prefix))));
            if (reach) {
                struct cd_host h = { "MDK2", "v1", hosts[j].name, CD_IN_LOBBY, 0, 0, 1, 4 };
                struct cd_reply rep;
                uint8_t reply[CD_REPLY_MAX];
                int n, at;
                reply_for(&h, 77, reply, &n);
                at = cd_match(&c, hosts[j].ip, hosts[j].port);
                if (at >= 0 && cd_parse_reply(reply, (size_t)n, 77, "MDK2", &rep) &&
                    cd_seen(list, &rep, c.c[at].route, hosts[j].ip, hosts[j].port, c.c[at].group, 5, 1000, "v1") >= 0) found++;
            }
        }
    }
    return found;
}

int main(int argc, char **argv)
{
    long fuzz = argc > 1 ? atol(argv[1]) : 20000, it;
    static char big[400000];
    uint8_t q[CD_QUERY_LEN], reply[CD_REPLY_MAX], copy[CD_REPLY_MAX + 2], lp_bytes[64];
    struct cd_host host = { "MDK2", "v0.3.0", "Fox", CD_IN_LOBBY, 0, 0, 2, 4 };
    struct cd_reply rep;
    struct cd_rate rate;
    struct cd_candidates c;
    struct cd_list list;
    struct cd_refresh refresh;
    struct cd_he he;
    static struct cd_ts ts;
    static struct cd_recents rs, rs2;
    struct lp_store ps;
    char text[CD_RECENT_BYTES];
    int n, i, k, allowed;

    /* query wire */
    CHECK(cd_query(q, 0x12345678u, "MDK2", 0) == 13 && !memcmp(q, "CPQ2\x78\x56\x34\x12MDK2\0", 13));
    CHECK(cd_query(q, 1, "WIT ", 0) == 13 && !memcmp(q + 8, "WIT ", 4) && !cd_query(q, 1, "WIT", 0));

    /* reply round trip */
    reply_for(&host, 0xabcdef01u, reply, &n);
    CHECK(n == 18 + 6 + 3 && !memcmp(reply, "CPS2\x01\xef\xcd\xabMDK2", 12));
    CHECK(cd_parse_reply(reply, (size_t)n, 0xabcdef01u, "MDK2", &rep) && rep.players == 2 && rep.limit == 4 &&
          rep.state == CD_IN_LOBBY && !strcmp(rep.version, "v0.3.0") && !strcmp(rep.name, "Fox") && !rep.flags);
    CHECK(!cd_parse_reply(reply, (size_t)n, 0xabcdef02u, "MDK2", &rep));            /* wrong nonce */
    CHECK(!cd_parse_reply(reply, (size_t)n, 0xabcdef01u, "WIT ", &rep));            /* wrong game */
    for (i = 0; i < n; i++) CHECK(!cd_parse_reply(reply, (size_t)i, 0xabcdef01u, "MDK2", &rep));
    memcpy(copy, reply, (size_t)n); copy[n] = 0; CHECK(!cd_parse_reply(copy, (size_t)n + 1, 0xabcdef01u, "MDK2", &rep));
    memcpy(copy, reply, (size_t)n); copy[12] = 3; CHECK(!cd_parse_reply(copy, (size_t)n, 0xabcdef01u, "MDK2", &rep));
    memcpy(copy, reply, (size_t)n); copy[14] = 0; CHECK(!cd_parse_reply(copy, (size_t)n, 0xabcdef01u, "MDK2", &rep));
    memcpy(copy, reply, (size_t)n); copy[13] = 5; CHECK(!cd_parse_reply(copy, (size_t)n, 0xabcdef01u, "MDK2", &rep));
    memcpy(copy, reply, (size_t)n); copy[18] = 7; CHECK(!cd_parse_reply(copy, (size_t)n, 0xabcdef01u, "MDK2", &rep));
    memcpy(copy, reply, (size_t)n); copy[n - 1] = (uint8_t)0xff; CHECK(!cd_parse_reply(copy, (size_t)n, 0xabcdef01u, "MDK2", &rep));
    memcpy(copy, reply, (size_t)n); copy[16] = 0; CHECK(!cd_parse_reply(copy, (size_t)n, 0xabcdef01u, "MDK2", &rep));
    memcpy(copy, reply, (size_t)n); copy[0] = 'X'; CHECK(!cd_parse_reply(copy, (size_t)n, 0xabcdef01u, "MDK2", &rep));
    memcpy(copy, reply, (size_t)n); copy[15] = CD_F_INVITE_ONLY;                       /* invite-only must carry no name */
    CHECK(!cd_parse_reply(copy, (size_t)n, 0xabcdef01u, "MDK2", &rep));
    /* host side: silent for other games, bad queries and bad state */
    cd_query(q, 5, "WIT ", 0); CHECK(!cd_answer(q, sizeof(q), &host, reply));
    cd_query(q, 5, "MDK2", 0); CHECK(!cd_answer(q, sizeof(q) - 1, &host, reply)); q[0] = 'X'; CHECK(!cd_answer(q, sizeof(q), &host, reply));
    host.version = ""; reply_for(&host, 5, reply, &n); CHECK(!n);
    host.version = "0123456789012345678901234567890123456789012345678"; reply_for(&host, 5, reply, &n); CHECK(!n);
    host.version = "v1"; host.players = 5; reply_for(&host, 5, reply, &n); CHECK(!n);
    host.players = 2; host.state = 3; reply_for(&host, 5, reply, &n); CHECK(!n);
    host.state = CD_IN_GAME; host.name = "A name that is far longer than thirty-two bytes";
    reply_for(&host, 5, reply, &n); CHECK(n > 0 && cd_parse_reply(reply, (size_t)n, 5, "MDK2", &rep) && strlen(rep.name) <= 32 && rep.state == CD_IN_GAME);
    /* B9: an invite-only host never puts its name in a reply */
    host.name = "Fox"; host.invite_only = 1; host.mismatch_possible = 1;
    reply_for(&host, 9, reply, &n);
    CHECK(n == 18 + 2 && reply[n - 1] == 0 && !memchr(reply, 'F', (size_t)n));
    CHECK(cd_parse_reply(reply, (size_t)n, 9, "MDK2", &rep) && rep.flags == (CD_F_INVITE_ONLY | CD_F_MISMATCH) && !rep.name[0]);
    host.invite_only = 0; host.mismatch_possible = 0; host.state = CD_IN_LOBBY;
    /* hosts keep answering CPQ1 exactly as v1 did */
    {
        unsigned char q1[8] = { 'C', 'P', 'Q', '1', 0x78, 0x56, 0x34, 0x12 };
        struct lp_probe p = { 0 };
        struct cd_host h16 = { "MDK2", "v1", "Fox", CD_IN_LOBBY, 0, 0, 3, 16 };
        int m = lp_answer(q1, 8, 3, 0, "v1", lp_bytes);
        n = cd_answer(q1, 8, &h16, reply);
        CHECK(n == m && !memcmp(reply, lp_bytes, (size_t)n));
        lp_sent(&p, 100, 0x12345678u);
        CHECK(lp_reply(&p, 110, reply, (size_t)n, 1, "v1") && p.status == LP_LOBBY);
        h16.limit = 40; h16.players = 30; n = cd_answer(q1, 8, &h16, reply);
        CHECK(n > 0 && reply[9] == 16 && reply[8] <= 16);
    }

    /* rate limit per source and in total */
    memset(&rate, 0, sizeof(rate));
    for (allowed = 0, i = 0; i < 30; i++) allowed += cd_rate_allow(&rate, IP(10, 0, 0, 1), 1000);
    CHECK(allowed == CD_RATE_PER_SOURCE);
    CHECK(!cd_rate_allow(&rate, IP(10, 0, 0, 1), 1050) && cd_rate_allow(&rate, IP(10, 0, 0, 1), 1100));
    CHECK(cd_rate_allow(&rate, IP(10, 0, 0, 2), 1100));
    for (allowed = 0, i = 0; i < 20; i++) allowed += cd_rate_allow(&rate, IP(10, 0, 0, 1), 1200 + (uint32_t)i * 100);
    CHECK(allowed == 20);                                               /* 10/s sustained */
    memset(&rate, 0, sizeof(rate));
    for (allowed = 0, i = 0; i < 1000; i++) allowed += cd_rate_allow(&rate, IP(10, 1, i >> 8, i & 255), 5000);
    CHECK(allowed == CD_RATE_TOTAL);                                    /* spoofed sources: total cap */
    memset(&rate, 0, sizeof(rate));
    for (allowed = 0, i = 0; i < 300; i++) allowed += cd_rate_allow(&rate, IP(10, 0, 0, 1), (uint32_t)-1000 + (uint32_t)i * 10);
    CHECK(allowed >= 10 && allowed <= 40);                              /* wrapping clock: still about 10/s */

    /* B1: directed broadcast on every adapter, default port and the next three */
    {
        struct cd_adapter ad[] = { { IP(192, 168, 1, 20), 24 }, { IP(10, 8, 0, 2), 24 }, { IP(127, 0, 0, 1), 8 },
                                   { IP(100, 101, 1, 1), 10 }, { IP(172, 16, 5, 9), 16 }, { IP(10, 1, 1, 1), 32 },
                                   { IP(192, 168, 1, 21), 24 } };
        struct sim_host hosts[] = { { IP(192, 168, 1, 30), 24, 27900, "Fox" }, { IP(10, 8, 0, 7), 24, 27901, "Owl" },
                                    { IP(172, 16, 200, 1), 16, 27903, "Badger" }, { IP(172, 16, 200, 2), 16, 27904, "Late" } };
        memset(&c, 0, sizeof(c));
        CHECK(cd_add_adapters(&c, ad, 7, 27900) == 12 && c.n == 12);
        for (i = 0; i < c.n; i++) {
            CHECK(c.c[i].ip != 0xffffffffu && c.c[i].broadcast && c.c[i].route == CD_LAN);
            CHECK(c.c[i].ip == IP(192, 168, 1, 255) || c.c[i].ip == IP(10, 8, 0, 255) || c.c[i].ip == IP(172, 16, 255, 255));
            CHECK(c.c[i].port >= 27900 && c.c[i].port <= 27903);
        }
        memset(&c, 0, sizeof(c)); CHECK(cd_add_adapters(&c, ad, 1, 65534) == 2);
        /* the second adapter's host is found; port 27904 is outside the four */
        CHECK(sim_scan(ad, 7, hosts, 4, 27900, &list) == 3 && list.n == 3);
        for (i = 0, k = 0; i < list.n; i++) k += !strcmp(list.r[i].name, "Owl") && list.r[i].route == CD_LAN && list.r[i].port == 27901;
        CHECK(k == 1);
    }
    /* reply sources: only addresses somebody asked */
    memset(&c, 0, sizeof(c));
    {
        struct cd_adapter ad[] = { { IP(192, 168, 1, 20), 24 } };
        cd_add_adapters(&c, ad, 1, 27900);
        cd_add(&c, IP(192, 168, 1, 30), 27900, CD_RECENT, 0, 0, -1);
        cd_add(&c, IP(100, 101, 1, 2), 27900, CD_TAILSCALE, 0, 0, -1);
        cd_add(&c, IP(8, 8, 4, 4), 27900, CD_INTERNET, 0, 0, 3);
        CHECK(!cd_add(&c, IP(8, 8, 4, 4), 27900, CD_INTERNET, 0, 0, 3) && !cd_add(&c, 0, 1, 0, 0, 0, -1) && !cd_add(&c, 1, 65536, 0, 0, 0, -1));
        CHECK(cd_match(&c, IP(192, 168, 1, 30), 27900) >= 0 && c.c[cd_match(&c, IP(192, 168, 1, 30), 27900)].route == CD_LAN);
        CHECK(c.c[cd_match(&c, IP(100, 101, 1, 2), 27900)].route == CD_TAILSCALE);
        CHECK(c.c[cd_match(&c, IP(8, 8, 4, 4), 27900)].group == 3);
        CHECK(cd_match(&c, IP(100, 101, 1, 9), 27900) < 0 && cd_match(&c, IP(8, 8, 8, 8), 27900) < 0);
        CHECK(cd_match(&c, IP(192, 168, 2, 30), 27900) < 0 && cd_match(&c, IP(192, 168, 1, 30), 27999) < 0);
        CHECK(cd_match(&c, IP(192, 168, 1, 255), 27900) < 0);
        for (i = 0; i < 600; i++) cd_add(&c, IP(10, 0, i >> 8, i & 255), 1000, CD_RECENT, 0, 0, -1);
        CHECK(c.n == CD_MAX_CANDIDATES && c.dropped > 0);
    }

    /* rows: one per host, best route, staleness */
    memset(&list, 0, sizeof(list));
    memset(&rep, 0, sizeof(rep)); strcpy(rep.name, "Fox"); strcpy(rep.version, "v1"); rep.players = 1; rep.limit = 4;
    CHECK(cd_seen(&list, &rep, CD_TAILSCALE, IP(100, 101, 1, 2), 27900, -1, 40, 1000, "v1") == 0);
    CHECK(cd_seen(&list, &rep, CD_LAN, IP(192, 168, 1, 30), 27900, -1, 2, 1100, "v1") == 0);
    CHECK(cd_seen(&list, &rep, CD_RECENT, IP(192, 168, 1, 30), 27900, -1, 3, 1100, "v1") == 0);
    CHECK(list.n == 1 && list.r[0].route == CD_LAN && list.r[0].rtt == 2 && list.r[0].ip == IP(192, 168, 1, 30) && !list.r[0].mismatch);
    CHECK(cd_seen(&list, &rep, CD_STEAM, 0, 27900, -1, 60, 1100, "v1") == 0 && list.r[0].route == CD_LAN);
    strcpy(rep.name, "Owl"); strcpy(rep.version, "v2");
    CHECK(cd_seen(&list, &rep, CD_LAN, IP(192, 168, 1, 31), 27900, -1, 2, 1100, "v1") == 1 && list.r[1].mismatch);
    strcpy(rep.name, "Fox");
    CHECK(cd_seen(&list, &rep, CD_LAN, IP(192, 168, 1, 32), 27900, -1, 2, 1100, "v1") == 2);  /* other version: other row */
    rep.flags = CD_F_INVITE_ONLY; rep.name[0] = 0;
    CHECK(cd_seen(&list, &rep, CD_LAN, IP(192, 168, 1, 40), 27900, -1, 2, 1100, "v1") < 0);   /* B9: not listed from a scan */
    CHECK(cd_seen(&list, &rep, CD_INTERNET, IP(8, 8, 4, 4), 27900, 7, 90, 1100, "v1") == 3);
    CHECK(cd_seen(&list, &rep, CD_TAILSCALE, IP(100, 101, 1, 7), 27900, 7, 30, 1150, "v1") == 3 && list.r[3].route == CD_TAILSCALE);
    CHECK(list.n == 4);
    rep.flags = 0; strcpy(rep.name, "Fox"); strcpy(rep.version, "v1");
    cd_seen(&list, &rep, CD_TAILSCALE, IP(100, 101, 1, 2), 27900, -1, 41, 9000, "v1");
    cd_seen(&list, &rep, CD_STEAM, 0, 27900, -1, 41, 9000, "v1");
    cd_expire(&list, 1100 + CD_STALE_MS);
    CHECK(list.n == 4 && list.r[0].route == CD_LAN);
    cd_expire(&list, 1151 + CD_STALE_MS);              /* LAN gone, Tailscale still fresh */
    CHECK(list.n == 1 && list.r[0].route == CD_TAILSCALE && list.r[0].rtt == 41 && !strcmp(list.r[0].name, "Fox"));
    cd_expire(&list, 9001 + CD_STALE_MS);
    CHECK(list.n == 0);
    for (i = 0; i < CD_ROWS + 5; i++) { snprintf(rep.name, sizeof(rep.name), "Host%d", i); cd_seen(&list, &rep, CD_LAN, (uint32_t)i + 1, 1, -1, 1, 1, "v1"); }
    CHECK(list.n == CD_ROWS);

    /* B6: scan as soon as shown, every 5 s, never while hidden */
    memset(&refresh, 0, sizeof(refresh));
    CHECK(!cd_refresh_due(&refresh, 0, 100) && cd_refresh_due(&refresh, 1, 100) && !cd_refresh_due(&refresh, 1, 101));
    CHECK(!cd_refresh_due(&refresh, 1, 100 + CD_REFRESH_MS - 1) && cd_refresh_due(&refresh, 1, 100 + CD_REFRESH_MS));
    CHECK(!cd_refresh_due(&refresh, 0, 20000) && !cd_refresh_due(&refresh, 0, 40000) && cd_refresh_due(&refresh, 1, 40001));
    refresh.last = 0xffffff00u; refresh.ever = 1;
    CHECK(!cd_refresh_due(&refresh, 1, 100) && cd_refresh_due(&refresh, 1, CD_REFRESH_MS));

    /* C4: first answer wins; a better route answering within 300 ms is preferred */
    {
        uint8_t routes[3] = { CD_INTERNET, CD_TAILSCALE, CD_LAN };
        cd_he_start(&he, routes, 3);
        CHECK(cd_he_choice(&he, 0) == -1);
        cd_he_reply(&he, 0, 1000); CHECK(cd_he_choice(&he, 1000) == -1);
        cd_he_reply(&he, 1, 1100); CHECK(cd_he_choice(&he, 1299) == -1 && cd_he_choice(&he, 1300) == 1);
        cd_he_start(&he, routes, 3);
        cd_he_reply(&he, 0, 1000); cd_he_reply(&he, 2, 1250); CHECK(cd_he_choice(&he, 1250) == 2);
        cd_he_start(&he, routes, 2);
        cd_he_reply(&he, 0, 1000); CHECK(cd_he_choice(&he, 1299) == -1 && cd_he_choice(&he, 1300) == 0);
        cd_he_reply(&he, 1, 1400); CHECK(cd_he_choice(&he, 1400) == 1);  /* caller has stopped by then */
        cd_he_start(&he, routes + 2, 1); cd_he_reply(&he, 0, 5); CHECK(cd_he_choice(&he, 5) == 0);
        cd_he_start(&he, routes, 3); cd_he_reply(&he, 9, 1); cd_he_reply(&he, -1, 1); CHECK(cd_he_choice(&he, 1000) == -1);
    }

    /* B2: tailscale status --json */
    CHECK(cd_ts_parse(ts_fixture, strlen(ts_fixture), &ts) && ts.running);
    CHECK(ts.total == 5 && ts.n == 2 && ts.offline == 1 && ts.no_ipv4 == 2);
    CHECK(ts.peer[0].ip == IP(100, 101, 1, 2) && !strcmp(ts.peer[0].host, "owl-laptop") && !strcmp(ts.peer[0].os, "windows") && !ts.peer[0].sharee);
    CHECK(ts.peer[1].ip == IP(100, 88, 7, 6) && ts.peer[1].sharee && !strcmp(ts.peer[1].host, "Zo\xc3\xab's PC \xf0\x9f\xa6\x8a"));
    CHECK(ts.self.ip == IP(100, 101, 1, 1) && !strcmp(ts.self.dns, "fox-pc.tail1234.ts.net") && !strcmp(ts.self.host, "fox-pc"));
    memset(&c, 0, sizeof(c));
    CHECK(cd_add_tailscale(&c, &ts, 27900) == 8 && c.c[0].route == CD_TAILSCALE && !c.c[0].broadcast);
    for (i = 0; i < c.n; i++) CHECK(c.c[i].ip == IP(100, 101, 1, 2) || c.c[i].ip == IP(100, 88, 7, 6));
    for (n = 0; n < (int)strlen(ts_fixture) - 2; n++) CHECK(!cd_ts_parse(ts_fixture, (size_t)n, &ts) && !ts.n && !ts.running);
    snprintf(big, sizeof(big), "%sx", ts_fixture); CHECK(!cd_ts_parse(big, strlen(big), &ts));
    CHECK(cd_ts_parse("{\"BackendState\":\"Stopped\",\"Peer\":{\"k\":{\"TailscaleIPs\":[\"100.1.1.1\"],\"Online\":true}}}", strlen("{\"BackendState\":\"Stopped\",\"Peer\":{\"k\":{\"TailscaleIPs\":[\"100.1.1.1\"],\"Online\":true}}}"), &ts) && !ts.running && !ts.n);
    CHECK(cd_ts_parse("{\"BackendState\":\"Running\",\"Peer\":null,\"Self\":null}", strlen("{\"BackendState\":\"Running\",\"Peer\":null,\"Self\":null}"), &ts) && ts.running && !ts.n);
    CHECK(cd_ts_parse(" {} ", strlen(" {} "), &ts) && !ts.running);
    CHECK(!cd_ts_parse("", strlen(""), &ts) && !cd_ts_parse("[]", strlen("[]"), &ts) && !cd_ts_parse("{\"a\":}", strlen("{\"a\":}"), &ts) && !cd_ts_parse("{\"a\":1,}", strlen("{\"a\":1,}"), &ts));
    CHECK(!cd_ts_parse("{\"a\":\"\\x\"}", strlen("{\"a\":\"\\x\"}"), &ts) && !cd_ts_parse("{\"a\":\"\\ud800\"}", strlen("{\"a\":\"\\ud800\"}"), &ts) && !cd_ts_parse("{\"a\":tru}", strlen("{\"a\":tru}"), &ts));
    CHECK(!cd_ts_parse("{\"a\":\"\x01\"}", strlen("{\"a\":\"\x01\"}"), &ts) && !cd_ts_parse("{\"a\":\"\xff\"}", strlen("{\"a\":\"\xff\"}"), &ts));
    /* a nesting bomb in an unknown field fails cleanly */
    n = sprintf(big, "{\"x\":");
    for (i = 0; i < 150000; i++) big[n++] = '[';
    for (i = 0; i < 150000; i++) big[n++] = ']';
    big[n++] = '}'; big[n] = 0;
    CHECK(!cd_ts_parse(big, (size_t)n, &ts));
    /* more peers than the table holds */
    n = sprintf(big, "{\"BackendState\":\"Running\",\"Peer\":{");
    for (i = 0; i < 300; i++) n += sprintf(big + n, "%s\"k%d\":{\"TailscaleIPs\":[\"100.64.%d.%d\"],\"Online\":true}", i ? "," : "", i, i >> 8, (i & 255) + 1);
    n += sprintf(big + n, "}}");
    CHECK(cd_ts_parse(big, (size_t)n, &ts) && ts.n == CD_TS_MAX && ts.total == 300);

    /* B4: recents store */
    memset(&rs, 0, sizeof(rs));
    CHECK(cd_recent_add(&rs, "Fox", "192.168.1.30", 27900, CD_R_LAN, 1000) == 0);
    CHECK(cd_recent_add(&rs, "Zo\xc3\xab", "owl-laptop.tail1234.ts.net", 27901, CD_R_TAILSCALE, 1001) == 1);
    CHECK(cd_recent_add(&rs, "Badger", "109775241234567890", 1, CD_R_STEAM, 1002) == 2);
    CHECK(cd_recent_add(&rs, "Fox again", "192.168.1.30", 27900, CD_R_LAN, 2000) == 0 && rs.n == 3 && rs.r[0].when == 2000 && !strcmp(rs.r[0].name, "Fox again"));
    CHECK(cd_recent_add(&rs, "x", "bad host", 1, CD_R_LAN, 1) < 0 && cd_recent_add(&rs, "x", "h", 0, CD_R_LAN, 1) < 0 && cd_recent_add(&rs, "x", "h", 1, 9, 1) < 0);
    CHECK(cd_recent_rename(&rs, 1, "Owl's laptop") && !cd_recent_rename(&rs, 1, "bad\tlabel") && !cd_recent_rename(&rs, 7, "x"));
    rs.r[2].pinned = 1; strcpy(rs.r[2].notes, "weekend games");
    n = cd_recents_encode(&rs, text, sizeof(text));
    CHECK(n > 0 && !strncmp(text, "COOP-RECENTS\t1\n\tFox again\t192.168.1.30\t27900\tlan\t2000\t0\t\n", 55));
    CHECK(cd_recents_decode(&rs2, text, (size_t)n) && rs2.n == 3 && !memcmp(&rs2.r[0], &rs.r[0], sizeof(rs.r[0])) && rs2.r[2].pinned &&
          !strcmp(rs2.r[1].label, "Owl's laptop") && !strcmp(rs2.r[1].name, "Zo\xc3\xab") && rs2.r[1].route == CD_R_TAILSCALE);
    CHECK(!cd_recents_encode(&rs, text, 40));
    /* transactional rejection */
    {
        const char *bad[] = { "COOP-RECENTS\t2\n", "COOP-RECENTS\t1\nl\tn\th\t1\tlan\t0\t0\t", "COOP-RECENTS\t1\nl\tn\th\t1\tlan\t0\t0\n\n",
                              "COOP-RECENTS\t1\nl\tn\th\t0\tlan\t0\t0\t\n", "COOP-RECENTS\t1\nl\tn\th\t1\tmoon\t0\t0\t\n",
                              "COOP-RECENTS\t1\nl\tn\th\t1\tlan\t0\t2\t\n", "COOP-RECENTS\t1\nl\tn\tb h\t1\tlan\t0\t0\t\n",
                              "COOP-RECENTS\t1\nl\tn\th\t1\tlan\t0\t0\t\nm\tn\tH\t1\tlan\t0\t0\t\n", "COOP-RECENTS\t1\nl\t\xff\th\t1\tlan\t0\t0\t\n",
                              "COOP-RECENTS\t1\nl\tn\th\t1\tlan\t0\t0\t\textra\n", "COOP-RECENTS\t1\nl\tn\t\t1\tlan\t0\t0\t\n",
                              "COOP-PROFILES\t1\nA\tip\t65536\t\t0\n", "COOP-PROFILES\t0\n\tip\t1\n" };
        struct cd_recents before = rs2;
        for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
            CHECK(!cd_recents_decode(&rs2, bad[i], strlen(bad[i])));
            CHECK(!memcmp(&before, &rs2, sizeof(rs2)));
        }
        memset(text, 'a', sizeof(text)); CHECK(!cd_recents_decode(&rs2, text, sizeof(text)));
    }
    /* migration from the saved-addresses store (COOP-PROFILES v1 and v0) */
    memset(&ps, 0, sizeof(ps)); ps.n = 2;
    strcpy(ps.p[0].name, "Friends"); strcpy(ps.p[0].host, "192.168.1.30"); ps.p[0].port = 27900; strcpy(ps.p[0].notes, "LAN party"); ps.p[0].used = 1234;
    strcpy(ps.p[1].name, "Server"); strcpy(ps.p[1].host, "example.org"); ps.p[1].port = 28000;
    n = lp_encode(&ps, text, sizeof(text));
    CHECK(n > 0 && cd_recents_decode(&rs2, text, (size_t)n) && rs2.n == 2);
    CHECK(!strcmp(rs2.r[0].label, "Friends") && !strcmp(rs2.r[0].host, "192.168.1.30") && rs2.r[0].port == 27900 &&
          !strcmp(rs2.r[0].notes, "LAN party") && rs2.r[0].when == 1234 && rs2.r[0].pinned && rs2.r[0].route == CD_R_ADDRESS && !rs2.r[0].name[0]);
    strcpy(text, "COOP-PROFILES\t0\nOld\tlocalhost\t10\n");
    CHECK(cd_recents_decode(&rs2, text, strlen(text)) && rs2.n == 1 && rs2.r[0].port == 10 && rs2.r[0].pinned && !rs2.r[0].when);
    n = cd_recents_encode(&rs2, text, sizeof(text)); CHECK(n > 0 && !strncmp(text, "COOP-RECENTS\t1\nOld\t\tlocalhost\t10\taddress\t0\t1\t\n", 47));
    /* eviction: oldest unpinned goes; all pinned refuses */
    memset(&rs, 0, sizeof(rs));
    for (i = 0; i < CD_RECENT_MAX; i++) { snprintf(text, 64, "10.0.0.%d", i + 1); CHECK(cd_recent_add(&rs, "h", text, 27900, CD_R_LAN, 100 + (uint64_t)i) == i); }
    rs.r[0].pinned = 1;
    CHECK(cd_recent_add(&rs, "new", "10.0.1.1", 27900, CD_R_LAN, 999) == CD_RECENT_MAX - 1 && rs.n == CD_RECENT_MAX);
    CHECK(!strcmp(rs.r[0].host, "10.0.0.1") && !strcmp(rs.r[1].host, "10.0.0.3"));
    for (i = 0; i < rs.n; i++) rs.r[i].pinned = 1;
    CHECK(cd_recent_add(&rs, "full", "10.0.2.1", 27900, CD_R_LAN, 1000) < 0);
    CHECK(cd_recent_forget(&rs, 0) && rs.n == CD_RECENT_MAX - 1 && !cd_recent_forget(&rs, 99));
    memset(&c, 0, sizeof(c));
    CHECK(cd_add_recent(&c, &rs.r[0], 0) == 1 && c.c[0].ip == IP(10, 0, 0, 3) && c.c[0].route == CD_RECENT);
    memset(&rs2, 0, sizeof(rs2)); cd_recent_add(&rs2, "x", "example.org", 1, CD_R_INTERNET, 1);
    CHECK(cd_add_recent(&c, &rs2.r[0], 0) == 0 && cd_add_recent(&c, &rs2.r[0], IP(93, 184, 216, 34)) == 1);
    /* invite routes become candidates in one group */
    {
        struct ci_invite v;
        memset(&v, 0, sizeof(v)); v.port = 27905; v.n = 2;
        v.route[0].kind = CI_LAN; memcpy(v.route[0].ip, "\xc0\xa8\x01\x05", 4);
        v.route[1].kind = CI_VPN; memcpy(v.route[1].ip, "\x64\x65\x66\x67", 4);
        memset(&c, 0, sizeof(c));
        CHECK(cd_add_invite(&c, &v, 4) == 2 && c.c[0].route == CD_LAN && c.c[1].route == CD_TAILSCALE && c.c[1].group == 4 && c.c[1].port == 27905);
    }

    /* fuzz: random and mutated input into every parser; nothing may crash, accepted replies are sane */
    for (it = 0; it < fuzz; it++) {
        size_t len = rnd() % 120, at;
        struct cd_host h = { "MDK2", "v1", "Fox", (int)(rnd() % 3), (int)(rnd() & 1), 0, 1, 4 };
        for (at = 0; at < len && at < sizeof(copy); at++) copy[at] = (uint8_t)rnd();
        if (rnd() & 1) { reply_for(&h, 42, copy, &n); len = (size_t)n; for (k = (int)(rnd() % 3); k >= 0; k--) copy[rnd() % len] = (uint8_t)rnd(); }
        if (cd_parse_reply(copy, len, 42, "MDK2", &rep)) {
            CHECK(rep.state <= 2 && rep.limit >= 1 && rep.players <= rep.limit && strlen(rep.version) >= 1 && strlen(rep.name) <= 32);
            CHECK(!(rep.flags & CD_F_INVITE_ONLY) || !rep.name[0]);
        }
        cd_answer(copy, len, &h, reply);
        len = rnd() % 600;
        if (rnd() & 1) { at = rnd() % strlen(ts_fixture); memcpy(big, ts_fixture, strlen(ts_fixture) + 1); len = strlen(big);
                         for (k = (int)(rnd() % 4); k >= 0; k--) big[rnd() % len] = (char)rnd(); }
        else for (at = 0; at < len; at++) big[at] = "{}[]\",:0123456789.eE-truefalsnl\\u \"Peer\"x"[rnd() % 40];
        if (cd_ts_parse(big, len, &ts)) CHECK(ts.n <= CD_TS_MAX && ts.n <= ts.total);
        len = rnd() % 300;
        memcpy(text, "COOP-RECENTS\t1\n", 15);
        for (at = 15; at < len; at++) text[at] = "\t\nab1.-9\xc3\xa9 "[rnd() % 11];
        if (cd_recents_decode(&rs2, text, len > 15 ? len : 15)) { char out[CD_RECENT_BYTES]; CHECK(cd_recents_encode(&rs2, out, sizeof(out)) > 0); }
    }
    printf("fuzz: %ld rounds of replies, status JSON and recents\n", fuzz);
    if (failures) { printf("FAILED: %d checks\n", failures); return 1; }
    puts("PASS: probe v2, CPQ1 answers, rate limit, per-adapter broadcast, rows, refresh, happy eyeballs, Tailscale JSON, recents");
    return 0;
}
