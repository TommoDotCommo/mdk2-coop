/* T0 for launcher_join.h: host answers (CPQ1 + CPQ2, rate limited), Join-list candidates on a model PC
 * with two adapters, Tailscale and recents, sources line, rows (B5) and their order, failures (F4),
 * invite routes at once (C4), recents migration and the rejoin shortcut (A4). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher_join.h"
#include "launcher_profiles.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
#define IP(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* One PC with several adapters, as in coop_discover_test.c: 255.255.255.255 leaves by the first
 * adapter only; a directed broadcast reaches the subnet of the adapter that owns it. */
struct sim_host { uint32_t ip; int prefix; unsigned port; const char *name, *version; };
static int sim_scan(const struct lj_scan_input *in, const struct sim_host *hosts, int nh, struct cd_list *list)
{
    struct cd_candidates c;
    int i, j, found = 0;
    memset(list, 0, sizeof(*list));
    lj_candidates(&c, in);
    for (j = 0; j < nh; j++) {
        int heard = 0;
        for (i = 0; i < c.n && !heard; i++) {
            uint32_t mask = cd_mask(hosts[j].prefix), bcast = hosts[j].ip | ~mask;
            const struct cd_adapter *first = &in->adapters[0];
            int reach = c.c[i].port == hosts[j].port &&
                        (c.c[i].ip == hosts[j].ip || c.c[i].ip == bcast ||
                         (c.c[i].ip == 0xffffffffu && (hosts[j].ip & cd_mask(first->prefix)) == (first->ip & cd_mask(first->prefix))));
            if (reach) {
                struct cd_host h = { "MDK2", hosts[j].version, hosts[j].name, CD_IN_LOBBY, 0, 0, 1, 16 };
                struct cd_rate rate;
                struct cd_reply rep;
                uint8_t q[CD_QUERY_LEN], reply[CD_REPLY_MAX];
                int n, at;
                memset(&rate, 0, sizeof(rate));
                cd_query(q, 99, "MDK2", 0);
                n = lj_host_reply(&rate, q, sizeof(q), 1, 0, &h, reply);
                at = cd_match(&c, hosts[j].ip, hosts[j].port);
                if (n && at >= 0 && cd_parse_reply(reply, (size_t)n, 99, "MDK2", &rep) &&
                    cd_seen(list, &rep, c.c[at].route, hosts[j].ip, hosts[j].port, c.c[at].group, 4, 1000, "v1") >= 0) {
                    found++; heard = 1;
                }
            }
        }
    }
    return found;
}

int main(void)
{
    char line[256];
    int i;

    /* 1. hosts answer CPQ1 exactly as before and CPQ2 for their game, rate limited per source */
    {
        struct cd_host h = { "MDK2", "v0.4.0", "Fox", CD_IN_LOBBY, 0, 0, 3, 16 };
        struct cd_rate rate;
        struct cd_reply rep;
        uint8_t q1[8] = { 'C', 'P', 'Q', '1', 1, 2, 3, 4 }, q2[CD_QUERY_LEN], reply[CD_REPLY_MAX];
        unsigned char old[64];
        int n, m, allowed = 0;
        memset(&rate, 0, sizeof(rate));
        n = lj_host_reply(&rate, q1, 8, IP(10, 0, 0, 2), 0, &h, reply);
        m = lp_answer(q1, 8, 3, 0, "v0.4.0", old);
        CHECK(n == m && n > 0 && !memcmp(reply, old, (size_t)n));
        cd_query(q2, 0xabcdef01u, "MDK2", 0);
        n = lj_host_reply(&rate, q2, sizeof(q2), IP(10, 0, 0, 2), 0, &h, reply);
        CHECK(n > 0 && cd_parse_reply(reply, (size_t)n, 0xabcdef01u, "MDK2", &rep) && !strcmp(rep.name, "Fox") &&
              rep.players == 3 && rep.limit == 16 && rep.state == CD_IN_LOBBY && !strcmp(rep.version, "v0.4.0"));
        cd_query(q2, 5, "WIT ", 0);
        CHECK(!lj_host_reply(&rate, q2, sizeof(q2), IP(10, 0, 0, 3), 0, &h, reply));
        CHECK(!lj_host_reply(&rate, (const uint8_t *)"MDKL H 1 2", 10, IP(10, 0, 0, 3), 0, &h, reply));
        memset(&rate, 0, sizeof(rate));
        cd_query(q2, 6, "MDK2", 0);
        for (i = 0; i < 40; i++) allowed += lj_host_reply(&rate, q2, sizeof(q2), IP(10, 0, 0, 9), 500, &h, reply) > 0;
        CHECK(allowed == CD_RATE_PER_SOURCE);
        CHECK(lj_host_reply(&rate, q2, sizeof(q2), IP(10, 0, 0, 10), 500, &h, reply) > 0);  /* another source still answered */
        h.invite_only = 1;
        n = lj_host_reply(&rate, q2, sizeof(q2), IP(10, 0, 0, 11), 500, &h, reply);
        CHECK(n > 0 && cd_parse_reply(reply, (size_t)n, 6, "MDK2", &rep) && !rep.name[0] && (rep.flags & CD_F_INVITE_ONLY));
        CHECK(!lj_host_reply(&rate, q1, 8, IP(10, 0, 0, 13), 500, &h, reply));    /* B9: not listed by the v1 probe */
        h.invite_only = 0; h.state = CD_IN_GAME;
        n = lj_host_reply(&rate, q2, sizeof(q2), IP(10, 0, 0, 12), 500, &h, reply);
        CHECK(n > 0 && cd_parse_reply(reply, (size_t)n, 6, "MDK2", &rep) && rep.state == CD_IN_GAME);
    }

    /* 2. ports: the configured one and the default, each with the next three */
    {
        unsigned p[2];
        CHECK(lj_ports(27916, p) == 2 && p[0] == 27916 && p[1] == 27900);
        CHECK(lj_ports(27900, p) == 1 && p[0] == 27900);
        CHECK(lj_ports(0, p) == 1 && p[0] == 27900);
        CHECK(lj_ports(65535, p) == 1 && p[0] == 27900);
    }

    /* 3. B1/B2/B4: two adapters, both hosts found; Tailscale peers only with the setting; recents probed */
    {
        struct cd_adapter ad[] = { { IP(192, 168, 1, 20), 24 }, { IP(10, 8, 0, 2), 24 }, { IP(100, 101, 1, 1), 10 } };
        struct sim_host hosts[] = { { IP(192, 168, 1, 30), 24, 27916, "Fox", "v1" }, { IP(10, 8, 0, 7), 24, 27917, "Owl", "v1" },
                                    { IP(100, 101, 1, 2), 10, 27900, "Badger", "v1" }, { IP(203, 0, 113, 5), 24, 28000, "Far", "v1" } };
        static struct cd_ts ts;
        static struct cd_recents rec;
        static struct cd_candidates c;
        static struct cd_list list;
        struct lj_scan_input in;
        memset(&ts, 0, sizeof(ts)); memset(&rec, 0, sizeof(rec)); memset(&in, 0, sizeof(in));
        ts.running = 1; ts.n = 1; ts.peer[0].ip = IP(100, 101, 1, 2); ts.peer[0].online = 1;
        in.adapters = ad; in.nadapters = 3; in.ts = &ts; in.tailscale_on = 1; in.recents = &rec;
        in.nports = lj_ports(27916, in.ports);
        CHECK(sim_scan(&in, hosts, 4, &list) == 3);    /* Fox on adapter 1, Owl on adapter 2, Badger on Tailscale */
        for (i = 0; i < list.n; i++) {
            if (!strcmp(list.r[i].name, "Fox") || !strcmp(list.r[i].name, "Owl")) CHECK(list.r[i].route == CD_LAN);
            if (!strcmp(list.r[i].name, "Badger")) CHECK(list.r[i].route == CD_TAILSCALE);
        }
        lj_candidates(&c, &in);
        for (i = 0; i < c.n; i++) CHECK(c.c[i].ip != 0xffffffffu);
        in.tailscale_on = 0;
        CHECK(sim_scan(&in, hosts, 4, &list) == 2);
        cd_recent_add(&rec, "Far", "203.0.113.5", 28000, CD_R_INTERNET, 100);
        CHECK(sim_scan(&in, hosts, 4, &list) == 3);
        for (i = 0; i < list.n; i++) if (!strcmp(list.r[i].name, "Far")) CHECK(list.r[i].route == CD_RECENT);
    }

    /* 4. B6 sources: named, ticked when done */
    {
        struct lj_source s = { LJ_S_LOOKING, 0 };
        CHECK(!lj_source_ticked(&s) && !strcmp(lj_source_text(LJ_SRC_LAN, &s, line, sizeof(line)), "Network..."));
        s.state = LJ_S_DONE; s.found = 2;
        CHECK(lj_source_ticked(&s) && !strcmp(lj_source_text(LJ_SRC_LAN, &s, line, sizeof(line)), "Network 2"));
        s.state = LJ_S_MISSING;
        CHECK(!lj_source_ticked(&s) && !strcmp(lj_source_text(LJ_SRC_TAILSCALE, &s, line, sizeof(line)), "Tailscale not installed"));
        s.state = LJ_S_INVITES;
        CHECK(!strcmp(lj_source_text(LJ_SRC_STEAM, &s, line, sizeof(line)), "Steam: invites"));
    }

    /* 5. B5 rows and their order */
    {
        static struct cd_list l;
        int idx[CD_ROWS];
        memset(&l, 0, sizeof(l));
        l.n = 4;
        strcpy(l.r[0].name, "Old"); strcpy(l.r[0].version, "v0.2.0"); l.r[0].mismatch = 1; l.r[0].players = 1; l.r[0].limit = 16; l.r[0].route = CD_LAN;
        strcpy(l.r[1].name, "Full"); strcpy(l.r[1].version, "v1"); l.r[1].players = 4; l.r[1].limit = 4; l.r[1].route = CD_LAN;
        strcpy(l.r[2].name, "Ts"); strcpy(l.r[2].version, "v1"); l.r[2].players = 1; l.r[2].limit = 16; l.r[2].route = CD_TAILSCALE; l.r[2].rtt = 2;   /* faster, but the worse route */
        strcpy(l.r[3].name, "Lan"); strcpy(l.r[3].version, "v1"); l.r[3].players = 2; l.r[3].limit = 16; l.r[3].route = CD_LAN; l.r[3].rtt = 3;
        l.r[3].state = CD_IN_GAME;
        lj_order(&l, idx);
        CHECK(idx[0] == 3 && idx[1] == 2 && idx[2] == 1 && idx[3] == 0);
        CHECK(lj_row_line(&l.r[3], line, sizeof(line)) && !strcmp(line, "in game  -  2 of 16  -  LAN  -  3 ms"));
        CHECK(lj_row_line(&l.r[1], line, sizeof(line)) && strstr(line, "full  -  4 of 4"));
        CHECK(lj_row_line(&l.r[0], line, sizeof(line)) && !strncmp(line, "version v0.2.0  -  in lobby", 27));
        l.r[2].name[0] = 0;
        CHECK(!strcmp(lj_row_name(&l.r[2]), "Unnamed host"));
    }

    /* 6. F4: who, what, next step */
    {
        int next = lj_failure(LJ_F_NO_ANSWER, "Fox", "LAN, Tailscale", line, sizeof(line));
        CHECK(strstr(line, "Fox") && strstr(line, "LAN, Tailscale") && (next & LJ_N_TRY_AGAIN) && (next & LJ_N_DIAGNOSTICS));
        next = lj_failure(LJ_F_VERSION, "Fox", "v0.4.0", line, sizeof(line));
        CHECK(!strcmp(line, "Fox has a different version (v0.4.0). Update to join.") && (next & LJ_N_UPDATE));
        next = lj_failure(LJ_F_FULL, "Fox", "16 of 16", line, sizeof(line));
        CHECK(strstr(line, "Fox's game is full (16 of 16)") && next == LJ_N_TRY_AGAIN);
        lj_failure(LJ_F_RESOLVE, "fox-pc", "Tailscale name", line, sizeof(line));
        CHECK(strstr(line, "fox-pc") && strstr(line, "Tailscale name"));
        lj_failure(LJ_F_NO_ANSWER, NULL, "LAN", line, sizeof(line));
        CHECK(strstr(line, "the host") != NULL);
    }

    /* 7. C4: every route at once, the first answer, a better one within 300 ms */
    {
        struct ci_invite v;
        static struct lj_invite_join j;
        memset(&v, 0, sizeof(v));
        v.port = 27900; v.n = 3;
        v.route[0].kind = CI_INTERNET; v.route[0].ip[0] = 203; v.route[0].ip[3] = 5;
        v.route[1].kind = CI_VPN; v.route[1].ip[0] = 100; v.route[1].ip[1] = 101; v.route[1].ip[3] = 2;
        v.route[2].kind = CI_LAN; v.route[2].ip[0] = 192; v.route[2].ip[1] = 168; v.route[2].ip[3] = 30;
        CHECK(lj_invite_start(&j, &v) == 3 && j.c.n == 3);
        CHECK(lj_invite_choice(&j, 0) == -1);
        lj_invite_reply(&j, 0, 1000);                      /* internet first */
        CHECK(lj_invite_choice(&j, 1100) == -1);           /* waits for a better route */
        lj_invite_reply(&j, 2, 1150);                      /* LAN within 300 ms: chosen at once */
        CHECK(lj_invite_choice(&j, 1150) == 2);
        lj_invite_start(&j, &v);
        lj_invite_reply(&j, 1, 1000);                      /* Tailscale only */
        CHECK(lj_invite_choice(&j, 1200) == -1 && lj_invite_choice(&j, 1300) == 1);
        lj_invite_start(&j, &v);
        lj_invite_reply(&j, 2, 50);                        /* the best route answers: no wait */
        CHECK(lj_invite_choice(&j, 50) == 2);
        CHECK(!strcmp(lj_routes_text(&v, line, sizeof(line)), "Internet, Tailscale, LAN"));
        v.route[1] = v.route[0];                           /* a duplicate route is tried once */
        CHECK(lj_invite_start(&j, &v) == 2 && j.candidate_of[1] == 1 && j.attempt_of[1] == 1);
        lj_invite_reply(&j, 1, 10);
        CHECK(lj_invite_choice(&j, 10) == 1);
    }

    /* 8. B4: recents load and migrate the saved addresses once */
    {
        static const char prof[] = "COOP-PROFILES\t1\nFriends\t192.168.1.30\t27900\tnotes\t100\n";
        static const char recs[] = "COOP-RECENTS\t1\n\tFox\t10.0.0.5\t27900\tlan\t50\t0\t\n";
        static struct cd_recents r;
        int migrated;
        CHECK(lj_recents_load(&r, NULL, 0, 0, prof, strlen(prof), 1, &migrated) && migrated && r.n == 1 &&
              r.r[0].pinned && !strcmp(r.r[0].label, "Friends") && r.r[0].route == CD_R_ADDRESS);
        CHECK(lj_recents_load(&r, recs, strlen(recs), 1, prof, strlen(prof), 1, &migrated) && !migrated && r.n == 1 &&
              !strcmp(r.r[0].name, "Fox"));
        CHECK(!lj_recents_load(&r, NULL, 0, 0, "junk", 4, 1, &migrated) && !migrated && r.n == 0);
        CHECK(lj_recents_load(&r, NULL, 0, 0, NULL, 0, 0, &migrated) && r.n == 0);
        CHECK(!lj_recents_load(&r, "junk", 4, 1, prof, strlen(prof), 1, &migrated));
        CHECK(lj_recent_route(CD_LAN, -1) == CD_R_LAN && lj_recent_route(CD_RECENT, CD_R_TAILSCALE) == CD_R_TAILSCALE &&
              lj_recent_route(CD_RECENT, -1) == CD_R_ADDRESS);
    }

    /* 9. A4: Rejoin <name> only when the last host answers with this version and room */
    {
        static struct cd_recents r;
        static struct cd_list l;
        memset(&r, 0, sizeof(r)); memset(&l, 0, sizeof(l));
        cd_recent_add(&r, "Owl", "10.0.0.9", 27900, CD_R_LAN, 10);
        cd_recent_add(&r, "Fox", "10.0.0.5", 27900, CD_R_LAN, 20);
        CHECK(lj_last_recent(&r) == 1);
        CHECK(!lj_rejoin_label(&l, &r, 0, line, sizeof(line)));
        l.n = 1; strcpy(l.r[0].name, "Fox"); l.r[0].port = 27900; l.r[0].players = 1; l.r[0].limit = 16;
        l.r[0].has[CD_LAN] = 1; l.r[0].ip_by[CD_LAN] = IP(10, 0, 0, 5); l.r[0].route = CD_LAN;
        CHECK(lj_rejoin_label(&l, &r, 0, line, sizeof(line)) && !strcmp(line, "Rejoin Fox"));
        l.r[0].mismatch = 1;
        CHECK(!lj_rejoin_label(&l, &r, 0, line, sizeof(line)));
        l.r[0].mismatch = 0; l.r[0].port = 27901;
        CHECK(!lj_rejoin_label(&l, &r, 0, line, sizeof(line)));
        l.r[0].port = 27900; l.r[0].players = 16;
        CHECK(!lj_rejoin_label(&l, &r, 0, line, sizeof(line)));
    }

    /* 10. C5: the handler link on the game's command line, and the join page link, both recognised */
    {
        char link[300], text[CI_CODE_MAX];
        struct cr_result r;
        struct ci_invite v;
        memset(&v, 0, sizeof(v));
        v.port = 27932; v.n = 1; v.route[0].kind = CI_LAN; v.route[0].ip[0] = 127; v.route[0].ip[3] = 1; v.secret = 7;
        CHECK(ci_encode(&v, "MDK2", text, sizeof(text)) > 0);
        snprintf(line, sizeof(line), "\"C:\\Games\\MDK2\\mdk2hd.exe\" \"MDK2-Coop://%s/\" -x", text);
        CHECK(lj_link_arg(line, link, sizeof(link)) && !strncmp(link, "mdk2-coop://", 12) && strstr(link, text) && !strchr(link, '"'));
        CHECK(coop_recognise(link, strlen(link), LJ_GAME, &r) == CR_LINK && r.invite.port == 27932 && r.invite.secret == 7);
        CHECK(!lj_link_arg("mdk2hd.exe +connect_lobby 1234", link, sizeof(link)) && !link[0]);
        CHECK(!lj_link_arg("mdk2hd.exe mdk2-coop:/x", link, sizeof(link)));
        CHECK(!lj_link_arg("x mdk2-coop://AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", link, 16));
        snprintf(link, sizeof(link), "https://tommodotcommo.github.io/coop-mod-kit/join/#%s", text);
        CHECK(coop_recognise(link, strlen(link), LJ_GAME, &r) == CR_LINK && r.invite.port == 27932);
    }

    if (failures) { printf("%d failures\n", failures); return 1; }
    puts("PASS: host answers, join-list candidates (two adapters, Tailscale, recents), sources, rows, order, failures, invite routes, recents migration, rejoin");
    return 0;
}
