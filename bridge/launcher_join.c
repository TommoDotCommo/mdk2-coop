/* The launcher's Join side (included by launcher.c, which owns the widgets and cfg):
 *
 *   Join a friend  "Games near you": one row per host found by directed broadcast on every adapter,
 *                  Tailscale peers (Settings > Network, on by default) and recent hosts, refreshed every
 *                  5 s while shown, each source named with a tick when done (B1-B6). Steam friends:
 *                  invites only (the launcher runs before Steam starts in this process, so friends' rich
 *                  presence cannot be read here; a Steam invite or a pasted lobby number joins).
 *   paste box      invite code, join-page link, Steam lobby number, address[:port], host or Tailscale
 *                  name; says what it recognised before Join (B7). An invite tries every route at once
 *                  and keeps the first answer, a better route within 300 ms instead (C4).
 *   clipboard      on launcher focus, this game's invite on the clipboard is offered once (B8); the
 *                  clipboard text is read for that check only, never stored, logged or sent.
 *   Rejoin <name>  on Play when the last host answers (A4).
 *   Recent hosts   remembered after each successful join, replacing the saved addresses (migrated
 *                  once); rename, pin, forget (B4).
 *   failures       name who and what and offer the next step (F4).
 *
 * The scan runs on a worker thread: adapters, `tailscale status --json`, host-name lookups and the UDP
 * scan never block the window. The UI reads results under a short lock. See docs/DISCOVERY.md. */
#include "launcher_join.h"
#include "coop_discover_win.h"

#define LJ_PASTE_MAX 256
#define LJ_TS_CACHE_MS 30000u
#define LJ_TS_JSON (1u << 20)

static void play_primary(void);
static int lj_text_box(int id, float x, float y, float w, const char *label, char *buf, int size, const char *placeholder);

enum { LJ_REQ_NONE, LJ_REQ_INVITE, LJ_REQ_NAME, LJ_REQ_ADDRESS };
enum { LJ_RES_NONE, LJ_RES_OK, LJ_RES_FAIL };
static CRITICAL_SECTION lj_lock;
static LONG lj_stop;
static HANDLE lj_thread;
static int lj_inited;
static volatile LONG lj_dirty;
static struct {
    /* UI -> worker */
    int shown, play_shown, find_lan, find_tailscale, port;
    struct cd_recents recents;
    int req, req_gen;
    struct ci_invite req_invite;
    char req_host[254], req_label[96];
    unsigned req_port;
    /* worker -> UI */
    struct cd_list list;
    struct lj_source src[LJ_SRCS];
    int scans, adapters, candidates;
    uint32_t recent_ip[CD_RECENT_MAX];
    int res, res_gen, res_route;
    uint32_t res_ip;
    unsigned res_port;
    struct cd_reply res_reply;
    char ts_self[128];
} lj;
/* UI only */
static struct {
    char paste[LJ_PASTE_MAX];
    struct cr_result rec;
    int selected, top, recents_sel, recents_top, renaming, busy, steps, migrated, offer;
    char renamed[49], message[220], busy_text[160];
    struct ci_invite offer_invite;
    char offer_name[CI_NAME_MAX + 1];
    uint32_t dismissed_secret; uint16_t dismissed_port; int dismissed;
    /* the join in progress: written to recents once it succeeded */
    int pending, pending_route;
    char pending_name[CI_NAME_MAX + 1], pending_host[128];
    unsigned pending_port;
    char shown_label[96];
} lju;

/* ---- recents store: the mod's data folder, recents.tsv (profiles.tsv migrated once) ---- */
static int lj_read(const char *path, char *bytes, size_t cap, size_t *n, int *exists)
{
    FILE *f = fopen(path, "rb");
    *n = 0; *exists = f != NULL;
    if (!f) return 0;
    *n = fread(bytes, 1, cap, f);
    fclose(f);
    return *n < cap;
}
static int lj_recents_save(void)
{
    static char bytes[CD_RECENT_BYTES];
    char path[MAX_PATH], stage[MAX_PATH];
    FILE *f;
    int n, ok;
    EnterCriticalSection(&lj_lock);
    n = cd_recents_encode(&lj.recents, bytes, sizeof(bytes));
    LeaveCriticalSection(&lj_lock);
    if (n <= 0) return 0;
    snprintf(path, sizeof(path), "%s\\recents.tsv", coop_dir);
    snprintf(stage, sizeof(stage), "%s\\recents.tsv.tmp", coop_dir);
    if (!(f = fopen(stage, "wb"))) return 0;
    ok = fwrite(bytes, 1, (size_t)n, f) == (size_t)n;
    if (fclose(f)) ok = 0;
    if (!ok || !MoveFileExA(stage, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileA(stage); return 0; }
    return 1;
}
static void lj_recents_load_files(void)
{
    static char rbytes[CD_RECENT_BYTES], pbytes[CD_RECENT_BYTES];
    static struct cd_recents loaded;
    char path[MAX_PATH];
    size_t rn, pn;
    int rhave, phave, rok, pok, migrated = 0;
    snprintf(path, sizeof(path), "%s\\recents.tsv", coop_dir);
    rok = lj_read(path, rbytes, sizeof(rbytes), &rn, &rhave);
    snprintf(path, sizeof(path), "%s\\profiles.tsv", coop_dir);
    pok = lj_read(path, pbytes, sizeof(pbytes), &pn, &phave);
    if ((rhave && !rok) || (!rhave && phave && !pok) ||
        !lj_recents_load(&loaded, rbytes, rn, rhave, pbytes, pn, phave, &migrated)) {
        memset(&loaded, 0, sizeof(loaded));
        snprintf(lju.message, sizeof(lju.message), "%s could not be read and was left as it is.", rhave ? "recents.tsv" : "profiles.tsv");
        logmsg("join: %s unreadable, kept", rhave ? "recents.tsv" : "profiles.tsv");
    }
    lj.recents = loaded;
    if (migrated) {
        lju.migrated = loaded.n;
        logmsg("join: %d saved address%s moved to recent hosts", loaded.n, loaded.n == 1 ? "" : "es");
        lj_recents_save();      /* profiles.tsv itself stays as it was */
    }
}

/* ---- the worker: scans while the list is shown ---- */
struct lj_scan_ctx { const struct cd_candidates *c; struct lj_invite_join *invite; LONG *early; };
static void lj_on_reply(void *ctx, int at, uint32_t ip, unsigned port, const struct cd_reply *rep, uint32_t rtt)
{
    struct lj_scan_ctx *x = (struct lj_scan_ctx *)ctx;
    EnterCriticalSection(&lj_lock);
    cd_seen(&lj.list, rep, x->c->c[at].route, ip, port, x->c->c[at].group, (int)rtt, GetTickCount(), MOD_VERSION);
    LeaveCriticalSection(&lj_lock);
    InterlockedExchange(&lj_dirty, 1);
}
static void lj_on_invite_reply(void *ctx, int at, uint32_t ip, unsigned port, const struct cd_reply *rep, uint32_t rtt)
{
    struct lj_scan_ctx *x = (struct lj_scan_ctx *)ctx;
    DWORD now = GetTickCount();
    lj_on_reply(ctx, at, ip, port, rep, rtt);
    lj_invite_reply(x->invite, at, now);
    if (lj_invite_choice(x->invite, now) >= 0) InterlockedExchange(x->early, 1);
}
static uint32_t lj_resolve(const char *host)
{
    struct addrinfo hints, *res = NULL;
    uint32_t ip = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (!getaddrinfo(host, NULL, &hints, &res) && res) ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    if (res) freeaddrinfo(res);
    return ip;
}
static void lj_result(int gen, int ok, uint32_t ip, unsigned port, int route, const struct cd_reply *rep)
{
    EnterCriticalSection(&lj_lock);
    if (lj.req_gen == gen) {
        lj.res = ok ? LJ_RES_OK : LJ_RES_FAIL; lj.res_gen = gen;
        lj.res_ip = ip; lj.res_port = port; lj.res_route = route;
        if (rep) lj.res_reply = *rep; else memset(&lj.res_reply, 0, sizeof(lj.res_reply));
        lj.req = LJ_REQ_NONE;
    }
    LeaveCriticalSection(&lj_lock);
    InterlockedExchange(&lj_dirty, 1);
}
/* One join request: an invite's routes at once (C4), or one resolved name / typed address. */
static void lj_do_request(int kind, int gen, const struct ci_invite *v, const char *host, unsigned port)
{
    static struct lj_invite_join ij;
    struct lj_scan_ctx x;
    LONG early = 0;
    uint32_t nonce = (uint32_t)rand() << 16 ^ (uint32_t)rand() ^ GetTickCount();
    int chosen;
    memset(&x, 0, sizeof(x));
    if (kind == LJ_REQ_INVITE) {
        if (!lj_invite_start(&ij, v)) { lj_result(gen, 0, 0, 0, 0, NULL); return; }
    } else {
        struct ci_invite one;
        uint32_t ip = lj_resolve(host);
        if (!ip) { lj_result(gen, 0, 0, 0, -1, NULL); return; }
        memset(&one, 0, sizeof(one));
        one.n = 1; one.port = (uint16_t)port;
        one.route[0].kind = cd_tailscale_ip(ip) ? CI_VPN : CI_LAN;
        one.route[0].ip[0] = (uint8_t)(ip >> 24); one.route[0].ip[1] = (uint8_t)(ip >> 16);
        one.route[0].ip[2] = (uint8_t)(ip >> 8); one.route[0].ip[3] = (uint8_t)ip;
        lj_invite_start(&ij, &one);
    }
    x.c = &ij.c; x.invite = &ij; x.early = &early;
    cdw_scan(&ij.c, nonce, LJ_PROBE_ID, CD_WINDOW_MS, 0, lj_on_invite_reply, &x, &early);
    chosen = lj_invite_choice(&ij, GetTickCount());
    if (chosen < 0 && ij.he.first >= 0) {
        DWORD wait = ij.he.first_at + CD_PREFER_MS - GetTickCount();
        if (wait < CD_PREFER_MS) Sleep(wait);
        chosen = lj_invite_choice(&ij, ij.he.first_at + CD_PREFER_MS);
    }
    if (chosen >= 0) {
        struct cd_reply rep;
        int i, found = 0;
        memset(&rep, 0, sizeof(rep));
        EnterCriticalSection(&lj_lock);
        for (i = 0; i < lj.list.n && !found; i++) {
            int k;
            for (k = 0; k < CD_ROUTES; k++)
                if (lj.list.r[i].has[k] && lj.list.r[i].ip_by[k] == ij.c.c[chosen].ip && lj.list.r[i].port == ij.c.c[chosen].port) {
                    const struct cd_row *r = &lj.list.r[i];
                    strcpy(rep.name, r->name); strcpy(rep.version, r->version);
                    rep.state = r->state; rep.players = r->players; rep.limit = r->limit; rep.flags = r->flags;
                    found = 1; break;
                }
        }
        LeaveCriticalSection(&lj_lock);
        lj_result(gen, 1, ij.c.c[chosen].ip, ij.c.c[chosen].port, ij.c.c[chosen].route, &rep);
    } else {
        lj_result(gen, 0, 0, 0, 0, NULL);
    }
}
static void lj_scan_once(void)
{
    static struct cdw_adapter wa[32];
    static struct cd_adapter ad[32];
    static struct cd_ts ts;
    static struct cd_candidates c;
    static struct cd_recents recents;
    static uint32_t recent_ip[CD_RECENT_MAX];
    static char *json;
    static DWORD ts_at;
    static int ts_state = LJ_S_IDLE, ts_have;
    struct lj_scan_input in;
    struct lj_scan_ctx x;
    int nad, i, find_lan, find_ts, port, play_only, sent, last = -1;
    int found[CD_ROUTES];
    uint32_t nonce = (uint32_t)rand() << 16 ^ (uint32_t)rand() ^ GetTickCount();

    EnterCriticalSection(&lj_lock);
    recents = lj.recents; find_lan = lj.find_lan; find_ts = lj.find_tailscale; port = lj.port;
    play_only = !lj.shown && lj.play_shown;
    if (!play_only) {   /* "looking" until a source's first answer; a refresh keeps its tick */
        int k;
        for (k = 0; k < LJ_SRC_STEAM; k++) {
            int on = k == LJ_SRC_LAN ? find_lan : k == LJ_SRC_TAILSCALE ? find_ts : 1, st = lj.src[k].state;
            lj.src[k].state = !on ? LJ_S_OFF : st == LJ_S_IDLE || st == LJ_S_OFF ? LJ_S_LOOKING : st;
        }
    }
    lj.src[LJ_SRC_STEAM].state = LJ_S_INVITES;
    LeaveCriticalSection(&lj_lock);
    InterlockedExchange(&lj_dirty, 1);
    memset(&in, 0, sizeof(in));
    /* Rejoin on Play: only the last host. The full list only while Join is shown. */
    if (play_only) {
        last = lj_last_recent(&recents);
        if (last < 0) return;
        recents.r[0] = recents.r[last]; recents.n = 1;
    }
    for (i = 0; i < recents.n; i++) {
        uint8_t ip[4];
        size_t used;
        recent_ip[i] = cr_ipv4(recents.r[i].host, strlen(recents.r[i].host), ip, &used) && used == strlen(recents.r[i].host)
                     ? 0 : recents.r[i].route == CD_R_STEAM ? 0 : lj_resolve(recents.r[i].host);
    }
    nad = 0;
    if (!play_only && find_lan) {
        int n = cdw_adapters(wa, 32);
        nad = cdw_adapter_list(wa, n < 0 ? 0 : n, ad);
    }
    if (!play_only && find_ts) {
        if (!ts_have || (DWORD)(GetTickCount() - ts_at) >= LJ_TS_CACHE_MS) {
            int n;
            if (!json) json = (char *)HeapAlloc(GetProcessHeap(), 0, LJ_TS_JSON);
            n = json ? cdw_tailscale_status(json, LJ_TS_JSON, 2000) : -1;
            ts_at = GetTickCount(); ts_have = 1;
            if (n == 0) { memset(&ts, 0, sizeof(ts)); ts_state = LJ_S_MISSING; }
            else if (n < 0 || !cd_ts_parse(json, (size_t)n, &ts)) { memset(&ts, 0, sizeof(ts)); ts_state = LJ_S_FAILED; }
            else ts_state = LJ_S_DONE;
            if (json && n > 0) SecureZeroMemory(json, (size_t)n);    /* peer names stay in the parsed rows only */
            EnterCriticalSection(&lj_lock);
            snprintf(lj.ts_self, sizeof(lj.ts_self), "%s", ts.self.dns);
            LeaveCriticalSection(&lj_lock);
        }
    }
    in.adapters = ad; in.nadapters = nad; in.ts = &ts; in.tailscale_on = !play_only && find_ts && ts_state == LJ_S_DONE;
    in.recents = &recents; in.recent_ip = recent_ip;
    in.nports = lj_ports((unsigned)port, in.ports);
    lj_candidates(&c, &in);
    memset(&x, 0, sizeof(x));
    x.c = &c;
    sent = c.n ? cdw_scan(&c, nonce, LJ_PROBE_ID, CD_WINDOW_MS, 0, lj_on_reply, &x, &lj_stop) : 0;
    EnterCriticalSection(&lj_lock);
    cd_expire(&lj.list, GetTickCount());
    memset(found, 0, sizeof(found));
    for (i = 0; i < lj.list.n; i++) if (lj.list.r[i].route < CD_ROUTES) found[lj.list.r[i].route]++;
    if (play_only) lj.recent_ip[last] = recent_ip[0];
    else for (i = 0; i < recents.n; i++) lj.recent_ip[i] = recent_ip[i];
    if (!play_only) {
        int recent_live = 0;
        for (i = 0; i < recents.n; i++) recent_live += lj_row_of_recent(&lj.list, &recents.r[i], recent_ip[i]) >= 0;
        lj.src[LJ_SRC_LAN].state = !find_lan ? LJ_S_OFF : nad ? (sent < 0 ? LJ_S_FAILED : LJ_S_DONE) : LJ_S_FAILED;
        lj.src[LJ_SRC_LAN].found = found[CD_LAN];
        lj.src[LJ_SRC_TAILSCALE].state = !find_ts ? LJ_S_OFF : ts_state;
        lj.src[LJ_SRC_TAILSCALE].found = found[CD_TAILSCALE];
        lj.src[LJ_SRC_RECENT].state = LJ_S_DONE;
        lj.src[LJ_SRC_RECENT].found = recent_live;
        lj.adapters = nad; lj.candidates = c.n;
    }
    lj.scans++;
    LeaveCriticalSection(&lj_lock);
    InterlockedExchange(&lj_dirty, 1);
}
static DWORD WINAPI lj_worker(LPVOID arg)
{
    WSADATA wsa;
    struct cd_refresh refresh = { 0, 0 };
    int was_full = 0;
    (void)arg;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 0;
    srand(GetTickCount() ^ GetCurrentProcessId());
    while (!InterlockedCompareExchange(&lj_stop, 0, 0)) {
        int shown, full, req, gen;
        struct ci_invite v;
        char host[254];
        unsigned port;
        EnterCriticalSection(&lj_lock);
        shown = lj.shown || lj.play_shown; full = lj.shown; req = lj.req; gen = lj.req_gen; v = lj.req_invite; port = lj.req_port;
        memcpy(host, lj.req_host, sizeof(host));
        LeaveCriticalSection(&lj_lock);
        if (full && !was_full) refresh.ever = 0;      /* B6: the list fills as soon as Join is chosen */
        was_full = full;
        if (req != LJ_REQ_NONE) lj_do_request(req, gen, &v, host, port);
        else if (cd_refresh_due(&refresh, shown, GetTickCount())) lj_scan_once();
        else Sleep(50);
    }
    WSACleanup();
    return 0;
}
static void lj_init(void)
{
    char s[16];
    if (lj_inited) return;
    lj_inited = 1;
    InitializeCriticalSection(&lj_lock);
    lj.find_lan = GetPrivateProfileIntA("net", "find_lan", 1, ini) != 0;
    lj.find_tailscale = GetPrivateProfileIntA("net", "find_tailscale", 1, ini) != 0;
    GetPrivateProfileStringA("net", "peer_port", "27900", s, sizeof(s), ini);
    lj.port = atoi(s);
    lju.recents_sel = lju.selected = -1;
    lj_recents_load_files();
    lj_thread = CreateThread(NULL, 0, lj_worker, NULL, 0, NULL);
    if (!lj_thread) snprintf(lju.message, sizeof(lju.message), "Could not start looking for games. Paste an invite or an address.");
}
static void lj_shutdown(void)
{
    if (!lj_inited) return;
    InterlockedExchange(&lj_stop, 1);
    if (lj_thread) { WaitForSingleObject(lj_thread, 4000); CloseHandle(lj_thread); lj_thread = NULL; }
}
/* What the window shows, set once per render: full = the Join list or Recent hosts (all sources),
 * play = Play without the list (only the last host, for Rejoin). Neither: nothing is sent. */
static void lj_set_view(int full, int play)
{
    lj_init();
    EnterCriticalSection(&lj_lock);
    if (lj.shown && !full) memset(lj.src, 0, sizeof(lj.src));    /* shown again: every source looks again */
    lj.shown = full;
    lj.play_shown = play && !full;
    LeaveCriticalSection(&lj_lock);
}
/* the window's timer: redraw when the worker found something */
static int lj_poll(void)
{
    if (!InterlockedExchange(&lj_dirty, 0)) return 0;
    changed = 1;
    return 1;
}

/* ---- joining ---- */
static void lj_fail(int kind, const char *who, const char *what)
{
    lju.steps = lj_failure(kind, who, what, lju.message, sizeof(lju.message));
    lju.busy = 0;
    logmsg("join: failed: %s", lju.message);
    changed = 1;
}
static void lj_note_pending(const char *name, const char *host, unsigned port, int route)
{
    lju.pending = 1; lju.pending_route = route; lju.pending_port = port;
    snprintf(lju.pending_name, sizeof(lju.pending_name), "%s", name ? name : "");
    snprintf(lju.pending_host, sizeof(lju.pending_host), "%s", host);
}
/* After a successful join: remember the host (B4). name: the host's name as the roster has it. */
static void lj_remember(const char *name, const char *host, unsigned port, int route)
{
    int i, previous = -1, at;
    EnterCriticalSection(&lj_lock);
    for (i = 0; i < lj.recents.n; i++)
        if (cd_same_host(lj.recents.r[i].host, host) && lj.recents.r[i].port == port) previous = lj.recents.r[i].route;
    at = cd_recent_add(&lj.recents, name ? name : "", host, port, lj_recent_route(route, previous), (uint64_t)time(NULL));
    LeaveCriticalSection(&lj_lock);
    if (at >= 0 && lj_recents_save()) logmsg("join: remembered %s (%s)", name && name[0] ? name : host,
                                            cd_recent_route_word(lj_recent_route(route, previous)));
}
/* launcher.c's lobby: the client is connected to the host whose roster names it host_name */
static void lj_joined(const char *host, unsigned port, const char *host_name)
{
    lj_init();
    if (lju.pending && !strcmp(lju.pending_host, host) && lju.pending_port == port)
        lj_remember(host_name && host_name[0] ? host_name : lju.pending_name, host, port, lju.pending_route);
    else
        lj_remember(host_name, host, port, CD_RECENT);
    lju.pending = 0;
}
/* The lobby's "Save address": a pinned recent host */
static int lj_save_endpoint(const char *host, unsigned port, const char *name)
{
    int at;
    lj_init();
    EnterCriticalSection(&lj_lock);
    at = cd_recent_add(&lj.recents, name ? name : "", host, port, CD_R_ADDRESS, (uint64_t)time(NULL));
    if (at >= 0) lj.recents.r[at].pinned = 1;
    LeaveCriticalSection(&lj_lock);
    return at >= 0 && lj_recents_save();
}
/* Join ip:port (UDP). In a lobby: through the launcher's lobby; in game: start the game and connect. */
static void lj_go(uint32_t ip, unsigned port, int state, const char *name, int route)
{
    char host[32];
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
    snprintf(cfg.host, sizeof(cfg.host), "%s", host);
    snprintf(cfg.port, sizeof(cfg.port), "%u", port);
    cfg.steam = 0; cfg.mode = MODE_JOIN;
    lju.busy = 0; lju.message[0] = 0; lju.steps = 0; lju.offer = 0;
    lj_note_pending(name, host, port, route);
    logmsg("join: %s (%s, %s)", name && name[0] ? name : "host", cd_route_name(route),
           state == CD_IN_GAME ? "in game" : "in lobby");
    if (state != CD_IN_GAME && lob_wants()) lob_open();
    else {
        lj_remember(name, host, port, route);          /* the game connects next; the lobby is skipped */
        done = 1;
    }
    changed = 1;
}
static int lj_row_go(const struct cd_row *r)
{
    char what[64];
    if (r->mismatch) { lj_fail(LJ_F_VERSION, lj_row_name(r), r->version); return 0; }
    if (lj_row_full(r)) { snprintf(what, sizeof(what), "%d of %d", r->players, r->limit); lj_fail(LJ_F_FULL, lj_row_name(r), what); return 0; }
    lj_go(r->ip, r->port, r->state, r->name, r->route);
    return 1;
}
static void lj_request(int kind, const struct ci_invite *v, const char *host, unsigned port, const char *label)
{
    lj_init();
    EnterCriticalSection(&lj_lock);
    lj.req = kind; lj.req_gen++; lj.res = LJ_RES_NONE;
    if (v) lj.req_invite = *v;
    else memset(&lj.req_invite, 0, sizeof(lj.req_invite));
    snprintf(lj.req_host, sizeof(lj.req_host), "%s", host ? host : "");
    snprintf(lj.req_label, sizeof(lj.req_label), "%s", label);
    lj.req_port = port;
    LeaveCriticalSection(&lj_lock);
    lju.busy = 1; lju.message[0] = 0; lju.steps = 0;
    snprintf(lju.busy_text, sizeof(lju.busy_text), "Looking for %s...", label);
    changed = 1;
}
static void lj_join_steam(uint64_t lobby, const char *why)
{
    cfg.mode = MODE_JOIN; cfg.steam = 1;
    snprintf(cfg.lobby, sizeof(cfg.lobby), "%llu", (unsigned long long)lobby);
    logmsg("join: %s", why);
    lju.busy = 0;
    done = 1;
}
static void lj_join_invite(const struct ci_invite *v, const char *name)
{
    char label[96];
    if (v->n) {
        snprintf(label, sizeof(label), "%s", name && name[0] ? name : "the host");
        lj_request(LJ_REQ_INVITE, v, NULL, 0, label);
        return;
    }
    if (v->flags & CI_FLAG_STEAM) { lj_join_steam(v->steam_lobby, "invite by Steam lobby"); return; }
    lj_fail(LJ_F_NO_ANSWER, name, "no routes in the invite");
}
/* the worker answered a request */
static void lj_take_result(void)
{
    int res, route, was_offer;
    uint32_t ip;
    unsigned port, req_port;
    struct cd_reply rep;
    struct ci_invite v;
    char label[96], what[96];
    if (!lju.busy) return;
    EnterCriticalSection(&lj_lock);
    res = lj.res_gen == lj.req_gen ? lj.res : LJ_RES_NONE;
    ip = lj.res_ip; port = lj.res_port; route = lj.res_route; rep = lj.res_reply; v = lj.req_invite; req_port = lj.req_port;
    memcpy(label, lj.req_label, sizeof(label));
    if (res != LJ_RES_NONE) lj.res = LJ_RES_NONE;
    LeaveCriticalSection(&lj_lock);
    if (res == LJ_RES_NONE) return;
    was_offer = lju.offer == 2;
    lju.offer = 0;
    if (res == LJ_RES_OK) {
        struct cd_row row;
        memset(&row, 0, sizeof(row));
        strcpy(row.name, rep.name); strcpy(row.version, rep.version);
        row.state = rep.state; row.players = rep.players; row.limit = rep.limit ? rep.limit : 16;
        row.mismatch = rep.version[0] && strcmp(rep.version, MOD_VERSION);
        row.ip = ip; row.port = (uint16_t)port; row.route = route;
        if (!row.name[0]) snprintf(row.name, sizeof(row.name), "%.32s", label);
        lju.busy = 0;
        lj_row_go(&row);
        return;
    }
    if (lju.rec.kind == CR_INVITE || lju.rec.kind == CR_LINK || was_offer) {
        if (v.flags & CI_FLAG_STEAM) { lj_join_steam(v.steam_lobby, "no UDP route answered; joining the invite's Steam lobby"); return; }
        lj_routes_text(&v, what, sizeof(what));
        lj_fail(LJ_F_NO_ANSWER, label, what);
    } else if (route < 0) {
        lj_fail(LJ_F_RESOLVE, label, lju.rec.kind == CR_TAILSCALE ? "Tailscale name" : "host name");
    } else {
        snprintf(what, sizeof(what), "port %u", req_port);
        lj_fail(LJ_F_NO_ANSWER, label, what);
    }
}
/* What the paste box holds, joined */
static void lj_join_paste(void)
{
    const struct cr_result *r = &lju.rec;
    unsigned port = r->port ? r->port : (unsigned)atoi(cfg.port) ? (unsigned)atoi(cfg.port) : LJ_DEFAULT_PORT;
    char label[96], text[160];
    switch (r->kind) {
    case CR_INVITE: case CR_LINK:
        lj_join_invite(&r->invite, NULL);
        break;
    case CR_STEAM_LOBBY:
        lj_join_steam(r->lobby, "Steam lobby typed");
        break;
    case CR_ADDRESS:
        snprintf(label, sizeof(label), "%u.%u.%u.%u:%u", r->ip[0], r->ip[1], r->ip[2], r->ip[3], port);
        snprintf(text, sizeof(text), "%u.%u.%u.%u", r->ip[0], r->ip[1], r->ip[2], r->ip[3]);
        lj_request(LJ_REQ_ADDRESS, NULL, text, port, label);
        break;
    case CR_HOST: case CR_TAILSCALE:
        snprintf(label, sizeof(label), "%s", r->host);
        lj_request(LJ_REQ_NAME, NULL, r->host, port, label);
        break;
    case CR_ERROR:
        lj_fail(LJ_F_CODE, NULL, cr_describe(r, text, sizeof(text)));
        break;
    default:
        break;
    }
}

/* ---- C5: started from the join page's "Open in launcher": the link goes into the paste box ---- */
static int lj_take_link(const char *cmdline)
{
    char link[LJ_PASTE_MAX];
    if (!lj_link_arg(cmdline, link, sizeof(link))) return 0;
    snprintf(lju.paste, sizeof(lju.paste), "%s", link);
    coop_recognise(lju.paste, strlen(lju.paste), LJ_GAME, &lju.rec);
    cfg.mode = MODE_JOIN;
    logmsg("launcher: started from an invite link (%s)", lju.rec.kind == CR_LINK ? "recognised" : "not recognised");
    return 1;
}

/* ---- B8: the clipboard on launcher focus ---- */
static void lj_clipboard_text(const char *text, size_t n)
{
    struct ci_invite v;
    char name[CI_NAME_MAX + 1];
    if (lju.offer || profile_session_active() || !ci_find_invite(text, n, LJ_GAME, &v, name)) return;
    if (lju.dismissed && v.secret == lju.dismissed_secret && v.port == lju.dismissed_port) return;   /* "Not now" stays until another invite */
    lju.offer = 1; lju.offer_invite = v;
    memcpy(lju.offer_name, name, sizeof(name));
    logmsg("join: an invite is on the clipboard");
    changed = 1;
}
static void lj_clipboard_check(HWND w)
{
    HANDLE h;
    const wchar_t *ws;
    char *utf8;
    int n;
    if (!OpenClipboard(w)) return;
    h = GetClipboardData(CF_UNICODETEXT);
    ws = h ? (const wchar_t *)GlobalLock(h) : NULL;
    if (ws && (utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, CI_SCAN_MAX + 1))) {
        n = WideCharToMultiByte(CP_UTF8, 0, ws, (int)wcsnlen(ws, CI_SCAN_MAX / 4), utf8, CI_SCAN_MAX, NULL, NULL);
        if (n > 0) lj_clipboard_text(utf8, (size_t)n);
        SecureZeroMemory(utf8, CI_SCAN_MAX + 1);       /* read for this check only, never kept */
        HeapFree(GetProcessHeap(), 0, utf8);
    }
    if (ws) GlobalUnlock(h);
    CloseClipboard();
}
static void lj_on_focus(HWND w)
{
    lj_init();
    lj_clipboard_check(w);
}

/* ---- drawing ---- */
/* a tick drawn with rectangles (the menu fonts have no check mark) */
static void lj_tick(float x, float y, DWORD c)
{
    int i;
    for (i = 0; i < 4; i++) fill(x + i, y + 5 + i, 2, 2, c, 255);
    for (i = 0; i < 7; i++) fill(x + 4 + i, y + 8 - i, 2, 2, c, 255);
}
static void lj_sources_line(float x, float y, const struct lj_source *src)
{
    int i;
    char t[48];
    for (i = 0; i < LJ_SRCS; i++) {
        lj_source_text(i, &src[i], t, sizeof(t));
        if (lj_source_ticked(&src[i])) { lj_tick(x, y + 3, C_BLUE); x += 14; }
        gdi_text(f_tiny, x, y, src[i].state == LJ_S_FAILED ? C_BAD : lj_source_ticked(&src[i]) ? C_TEXT : C_DIM, t, DT_LEFT);
        x += gdi_width(f_tiny, t) + 14;
    }
}
/* F4: the message and its next steps as buttons */
static float lj_failure_panel(float y)
{
    float x = PX;
    if (!lju.message[0]) return y;
    y += gdi_para(f_small, PX, y, PW, lju.steps ? C_BAD : C_DIM, lju.message) + 4;
    if (!lju.steps) return y;
    if (lju.steps & LJ_N_TRY_AGAIN) {
        if (button(ID, x, y, 120, 28, "Try again", 0)) { lju.message[0] = 0; lju.steps = 0; play_primary(); }
        x += 128;
    }
    if (lju.steps & LJ_N_UPDATE) {
        if (button(ID, x, y, 100, 28, "Update", 0)) { update_init(); update_start_check(1); page = PAGE_ABOUT; changed = 1; }
        x += 108;
    }
    if (lju.steps & LJ_N_DIAGNOSTICS) {
        if (button(ID, x, y, 170, 28, "Send diagnostics", 0)) {
            if (diag_state != 1) diag_state = diag_bundle_start() ? 1 : 3;
            page = PAGE_HELP; changed = 1;
        }
    }
    return y + 34;
}
/* B8 offer, drawn instead of the mode's options while it waits for an answer. 1 = drawn. */
static int lj_offer_panel(float y)
{
    char line[160];
    lj_take_result();
    if (!lju.offer || profile_session_active()) return 0;
    ci_clipboard_prompt(lju.offer_name, line, sizeof(line));
    fill(PX, y, PW, 120, 0x1c2a20, 235);
    frame_rect(PX, y, PW, 120, 1, C_ORANGE, 255);
    gdi_para(f_body, PX + 16, y + 14, PW - 32, C_TEXT, line);
    if (lju.busy) gdi_text(f_small, PX + 16, y + 48, C_DIM, lju.busy_text, DT_LEFT);
    if (button(ID, PX + 16, y + 72, 140, 34, "Join", 1)) play_primary();
    if (button(ID, PX + 168, y + 72, 140, 34, "Not now", 0)) {
        lju.dismissed = 1; lju.dismissed_secret = lju.offer_invite.secret; lju.dismissed_port = lju.offer_invite.port;
        lju.offer = 0; lju.busy = 0; changed = 1;
    }
    lj_failure_panel(y + 128);
    return 1;
}
/* Join a friend: the paste box, the sources and "Games near you" (rows of the last scans) */
static float lj_join_panel(float y)
{
    static struct cd_list list;
    static struct lj_source src[LJ_SRCS];
    int idx[CD_ROWS], i, n, shown = 3;
    char line[200], desc[200];

    lj_init();
    lj_take_result();
    EnterCriticalSection(&lj_lock);
    list = lj.list;
    memcpy(src, lj.src, sizeof(src));
    LeaveCriticalSection(&lj_lock);

    if (lj_text_box(ID, PX, y, PW, "Invite, link, Steam lobby or address", lju.paste, sizeof(lju.paste),
                    "Paste an invite, or type an address")) {
        coop_recognise(lju.paste, strlen(lju.paste), LJ_GAME, &lju.rec);
        lju.message[0] = 0; lju.steps = 0;
        if (lju.rec.kind != CR_EMPTY) lju.selected = -1;
    }
    y += 66;
    if (lju.rec.kind != CR_EMPTY) {
        cr_describe(&lju.rec, desc, sizeof(desc));
        clip_text(desc, PW);
        gdi_text(f_small, PX, y, lju.rec.kind == CR_ERROR ? C_BAD : C_BLUE, desc, DT_LEFT);
    } else {
        heading(PX, y - 2, 18, "Games near you", C_BLUE, DT_LEFT);
    }
    y += 22;
    lj_sources_line(PX, y, src);
    y += 20;
    if (lju.busy) {
        gdi_text(f_small, PX, y + 4, C_DIM, lju.busy_text, DT_LEFT);
        return y + 30;
    }
    if (lju.message[0]) { y = lj_failure_panel(y + 2); shown = 1; }
    n = lj_order(&list, idx);
    if (lju.top > 0 && lju.top >= n) lju.top = 0;
    if (!n) {
        gdi_para(f_small, PX, y + 6, PW, C_FAINT, src[LJ_SRC_LAN].state == LJ_S_LOOKING && !src[LJ_SRC_LAN].found ?
                 "Looking on your networks..." :
                 "No games found yet. Ask the host to open their lobby, or paste their invite above.");
    }
    for (i = lju.top; i < n && i < lju.top + shown; i++) {
        const struct cd_row *r = &list.r[idx[i]];
        int on = lju.selected == idx[i], grey = r->mismatch || lj_row_full(r), hit;
        float h = 44;
        hit = control(ID + 9000 + i, PX, y, PW, h);
        fill(PX, y, PW, h - 2, on ? 0x2a2014 : over(PX, y, PW, h) ? C_TILE_HOT : C_TILE, 235);
        fill(PX, y, 5, h - 2, on ? C_ORANGE : grey ? 0x2a3a42 : C_BLUE, 255);
        if (on) frame_rect(PX, y, PW, h - 2, 2, C_ORANGE, 255);
        snprintf(line, sizeof(line), "%s", lj_row_name(r));
        clip_text(line, PW - 40);
        gdi_text(f_body, PX + 16, y + 2, grey ? C_DIM : on ? C_ORANGE : C_TEXT, line, DT_LEFT);
        lj_row_line(r, line, sizeof(line));
        clip_text(line, PW - 30);
        gdi_text(f_tiny, PX + 16, y + 24, grey ? C_FAINT : C_DIM, line, DT_LEFT);
        focus_ring(ID + 9000 + i, PX, y, PW, h - 2);
        if (hit) {
            if (on && clicked) play_primary();         /* a second click on the chosen row joins */
            else { lju.selected = idx[i]; snprintf(lju.shown_label, sizeof(lju.shown_label), "%s", lj_row_name(r)); }
            lju.paste[0] = 0; coop_recognise("", 0, LJ_GAME, &lju.rec);
            lju.message[0] = 0; lju.steps = 0;
            changed = 1;
        }
        y += h;
    }
    if (n > shown) {
        snprintf(line, sizeof(line), "%d-%d of %d", lju.top + 1, lju.top + shown < n ? lju.top + shown : n, n);
        gdi_text(f_tiny, PX + PW - 60, y + 4, C_DIM, line, DT_RIGHT);
        if (link(ID, PX + PW - 50, y, "More")) { lju.top = lju.top + shown < n ? lju.top + shown : 0; changed = 1; }
    }
    if (link(ID, PX, y, "Recent hosts")) { page = PAGE_PROFILES; changed = 1; }
    return y + 30;
}
static int lj_rejoin(char *label, size_t cap, int go)
{
    static struct cd_list list;
    static struct cd_recents rec;
    uint32_t resolved;
    int last, row, ok;
    lj_init();
    EnterCriticalSection(&lj_lock);
    list = lj.list; rec = lj.recents;
    last = lj_last_recent(&rec);
    resolved = last >= 0 ? lj.recent_ip[last] : 0;
    LeaveCriticalSection(&lj_lock);
    ok = lj_rejoin_label(&list, &rec, resolved, label, cap);
    if (ok && go && (row = lj_row_of_recent(&list, &rec.r[last], resolved)) >= 0) lj_row_go(&list.r[row]);
    return ok;
}
/* The Rejoin button on Play (A4), above the primary button, when not on the Join list */
static void lj_rejoin_button(float x, float y, float w)
{
    char label[80];
    if (profile_session_active() || cfg.mode == MODE_JOIN || lju.offer || !lj_rejoin(label, sizeof(label), 0)) return;
    clip_text(label, w - 20);
    if (button(ID, x, y, w, 30, label, 0)) lj_rejoin(label, sizeof(label), 1);
}

/* The primary button's words on Play in Join mode (A2: says what it does) */
static const char *lj_primary_label(char *out, size_t cap)
{
    if (lju.offer) {
        snprintf(out, cap, "Join %s", lju.offer_name[0] ? lju.offer_name : "invite");
        return out;
    }
    if (lju.rec.kind == CR_INVITE || lju.rec.kind == CR_LINK) snprintf(out, cap, "Join invite");
    else if (lju.rec.kind == CR_STEAM_LOBBY) snprintf(out, cap, "Join Steam lobby");
    else if (lju.rec.kind == CR_ADDRESS || lju.rec.kind == CR_HOST || lju.rec.kind == CR_TAILSCALE) snprintf(out, cap, "Join it");
    else if (lju.selected >= 0) {
        int ok;
        EnterCriticalSection(&lj_lock);
        ok = lju.selected < lj.list.n;
        snprintf(out, cap, "Join %s", ok ? lj_row_name(&lj.list.r[lju.selected]) : lju.shown_label);
        LeaveCriticalSection(&lj_lock);
    } else if (cfg.steam) snprintf(out, cap, "Join over Steam");
    else snprintf(out, cap, "Join");
    return out;
}
/* The primary action in Join mode */
static void lj_primary(void)
{
    static struct cd_row row;
    int ok;
    lj_init();
    if (lju.busy) return;
    if (lju.offer) {
        lju.offer = 2;
        cfg.mode = MODE_JOIN;          /* the answer and any failure show on the Join page */
        lj_join_invite(&lju.offer_invite, lju.offer_name);
        if (lju.offer == 2 && !lju.busy) lju.offer = 0;
        return;
    }
    if (lju.rec.kind != CR_EMPTY) { lj_join_paste(); return; }
    if (lju.selected >= 0) {
        EnterCriticalSection(&lj_lock);
        ok = lju.selected < lj.list.n;
        if (ok) row = lj.list.r[lju.selected];
        LeaveCriticalSection(&lj_lock);
        if (ok) { lj_row_go(&row); return; }
        lj_fail(LJ_F_NO_ANSWER, lju.shown_label, "it stopped answering");
        lju.selected = -1;
        return;
    }
    /* Nothing chosen: the address the launcher used last (Steam: the game takes the Steam invite) */
    if (cfg.steam || cfg.host[0]) {
        if (!cfg.steam) lj_note_pending("", cfg.host, (unsigned)atoi(cfg.port), CD_RECENT);
        if (lob_wants()) lob_open(); else done = 1;
        return;
    }
    lj_fail(LJ_F_NOTHING, NULL, NULL);
}

/* ---- Recent hosts (B4): rename, pin, forget, join ---- */
static void lj_page_recents(void)
{
    static struct cd_recents rec;
    static struct cd_list list;
    uint32_t ips[CD_RECENT_MAX];
    float y = 140;
    int i, live;
    char line[220], when[32];

    lj_init();
    lj_take_result();
    EnterCriticalSection(&lj_lock);
    rec = lj.recents; list = lj.list;
    memcpy(ips, lj.recent_ip, sizeof(ips));
    LeaveCriticalSection(&lj_lock);
    heading(PX, 96, 22, "Recent hosts", C_BLUE, DT_LEFT);
    if (link(ID, PX + PW - 42, 100, "Back")) { lju.renaming = 0; page = PAGE_PLAY; changed = 1; }
    if (!rec.n) {
        gdi_para(f_small, PX, y, PW, C_DIM, "Hosts you join are remembered here, with the way that worked. "
                 "Pin one to keep it; the oldest unpinned host goes when the list is full.");
    }
    if (lju.recents_top >= rec.n) lju.recents_top = 0;
    for (i = lju.recents_top; i < rec.n && i < lju.recents_top + 4; i++) {
        const struct cd_recent *e = &rec.r[i];
        int row = lj_row_of_recent(&list, e, ips[i]);
        live = row >= 0;
        ago((long)e->when, when, sizeof(when));
        if (live) snprintf(line, sizeof(line), "%s:%u  -  %s  -  %s  -  %s%s", e->host, e->port, lj_state_text(&list.r[row]),
                           cd_route_name(list.r[row].route), when, e->pinned ? "  -  pinned" : "");
        else snprintf(line, sizeof(line), "%s:%u  -  no answer  -  %s, %s%s", e->host, e->port, cd_recent_route_word(e->route),
                      when, e->pinned ? "  -  pinned" : "");
        clip_text(line, PW - 36);
        if (tile(ID + 9100 + i, PX, y, PW, 66, lj_recent_label(e), line, lju.recents_sel == i)) {
            lju.recents_sel = i; lju.renaming = 0; changed = 1;
        }
        y += 72;
    }
    y = 446;
    if (link(ID, PX, y, "Previous") && lju.recents_top > 0) { lju.recents_top -= 4; changed = 1; }
    if (link(ID, PX + 110, y, "Next") && lju.recents_top + 4 < rec.n) { lju.recents_top += 4; changed = 1; }
    y += 30;
    if (lju.recents_sel >= 0 && lju.recents_sel < rec.n) {
        int s = lju.recents_sel;
        if (lju.renaming) {
            lj_text_box(ID, PX, y, PW - 230, "", lju.renamed, sizeof(lju.renamed), lj_recent_label(&rec.r[s]));
            if (button(ID, PX + PW - 220, y, 105, 38, "Save", 1)) {
                EnterCriticalSection(&lj_lock);
                if (s < lj.recents.n) cd_recent_rename(&lj.recents, s, lju.renamed);
                LeaveCriticalSection(&lj_lock);
                lj_recents_save(); lju.renaming = 0; changed = 1;
            }
            if (button(ID, PX + PW - 105, y, 105, 38, "Cancel", 0)) { lju.renaming = 0; changed = 1; }
        } else {
            if (button(ID, PX, y, 100, 38, "Join", 1)) {
                uint8_t ip[4];
                size_t used;
                const struct cd_recent *e = &rec.r[s];
                int row = lj_row_of_recent(&list, e, ips[s]);
                cfg.mode = MODE_JOIN;
                if (row >= 0) lj_row_go(&list.r[row]);
                else if (cr_ipv4(e->host, strlen(e->host), ip, &used) && used == strlen(e->host))
                    lj_request(LJ_REQ_ADDRESS, NULL, e->host, e->port, lj_recent_label(e));
                else lj_request(LJ_REQ_NAME, NULL, e->host, e->port, lj_recent_label(e));
                page = PAGE_PLAY;
                changed = 1;
            }
            if (button(ID, PX + 110, y, 110, 38, "Rename", 0)) {
                snprintf(lju.renamed, sizeof(lju.renamed), "%s", rec.r[s].label);
                lju.renaming = 1; changed = 1;
            }
            if (button(ID, PX + 230, y, 110, 38, rec.r[s].pinned ? "Unpin" : "Pin", 0)) {
                EnterCriticalSection(&lj_lock);
                if (s < lj.recents.n) lj.recents.r[s].pinned = !lj.recents.r[s].pinned;
                LeaveCriticalSection(&lj_lock);
                lj_recents_save(); changed = 1;
            }
            if (button(ID, PX + 350, y, 110, 38, "Forget", 0)) {
                EnterCriticalSection(&lj_lock);
                if (s < lj.recents.n) cd_recent_forget(&lj.recents, s);
                LeaveCriticalSection(&lj_lock);
                lj_recents_save(); lju.recents_sel = -1; changed = 1;
            }
        }
    }
    if (lju.migrated)
        snprintf(line, sizeof(line), "Your %d saved address%s here now, pinned.", lju.migrated, lju.migrated == 1 ? " is" : "es are");
    else snprintf(line, sizeof(line), "Kept on this PC only, never sent anywhere.");
    gdi_para(f_small, PX, 526, PW, C_DIM, lju.message[0] ? lju.message : line);
}

/* ---- Settings > Network (E): port, find games on my network, find friends on Tailscale ---- */
static float lj_settings_network(float y)
{
    static char port[8];
    int lan, tsn;
    lj_init();
    if (!port[0]) snprintf(port, sizeof(port), "%s", cfg.port);
    if (lj_text_box(ID, PX, y, 160, "Port", port, sizeof(port), "27900")) {
        int p = atoi(port);
        if (p > 0 && p <= 65535) {
            snprintf(cfg.port, sizeof(cfg.port), "%d", p); snprintf(cfg.listen, sizeof(cfg.listen), "%d", p);
            put("net", "peer_port", cfg.port); put("net", "listen_port", cfg.listen);
            EnterCriticalSection(&lj_lock); lj.port = p; LeaveCriticalSection(&lj_lock);
        }
    }
    gdi_para(f_small, PX + 176, y + 26, PW - 176, C_DIM, "UDP, for hosting and joining. Games are looked for on it and the next three.");
    y += 76;
    EnterCriticalSection(&lj_lock); lan = lj.find_lan; tsn = lj.find_tailscale; LeaveCriticalSection(&lj_lock);
    if (toggle(ID, PX, y, PW, "Find games on my network", "Every network this PC is on", &lan) || lan != lj.find_lan) {
        put_int("net", "find_lan", lan);
        EnterCriticalSection(&lj_lock); lj.find_lan = lan; LeaveCriticalSection(&lj_lock);
    }
    y += ROW + 2;
    if (toggle(ID, PX, y, PW, "Find friends on Tailscale", "Asks Tailscale on this PC for your devices", &tsn) || tsn != lj.find_tailscale) {
        put_int("net", "find_tailscale", tsn);
        EnterCriticalSection(&lj_lock); lj.find_tailscale = tsn; LeaveCriticalSection(&lj_lock);
    }
    return y + ROW + 2;
}

/* A text box that shows the end of long text (invite links); Enter on Play does the primary action */
static int lj_text_box(int id, float x, float y, float w, const char *label, char *buf, int size, const char *placeholder)
{
    int edited = 0, i, start = 0;
    char shown[LJ_PASTE_MAX + 4];
    if (!label[0]) y -= 24;                 /* no label: the box itself at y */
    if (focus == id) field_focused = 1;
    if (control(id, x, y + 24, w, 40) && !clicked && page == PAGE_PLAY && !profile_session_active()) play_primary();
    if (label[0]) gdi_text(f_small, x, y, C_DIM, label, DT_LEFT);
    if (focus == id) {
        for (i = 0; i < ntyped; i++) {
            int n = (int)strlen(buf);
            char c = typed[i];
            if (c == '\b') { if (n) buf[n - 1] = 0; }
            else if (n < size - 1 && c >= 32 && c < 127) { buf[n] = c; buf[n + 1] = 0; }
            else continue;
            edited = 1;
        }
        ntyped = 0;
    }
    fill(x, y + 24, w, 40, focus == id ? 0x0d1a20 : C_TILE, 245);
    fill(x, y + 62, w, 2, focus == id ? C_BLUE : C_FAINT, 255);
    while (buf[start] && gdi_width(f_body, buf + start) > w - 30) start++;
    snprintf(shown, sizeof(shown), "%s%s", start ? "..." : "", buf + start);
    if (buf[0]) gdi_text(f_body, x + 12, y + 33, C_TEXT, shown, DT_LEFT);
    else if (placeholder) gdi_text(f_body, x + 12, y + 33, C_FAINT, placeholder, DT_LEFT);
    if (focus == id && caret_on) fill(x + 13 + (buf[0] ? gdi_width(f_body, shown) : 0), y + 33, 2, 24, C_BLUE, 255);
    if (edited) changed = 1;
    return edited;
}

/* ---- [test] launcher=1: the state for tools (dump) ---- */
static void lj_dump(FILE *f)
{
    int i;
    char line[200];
    lj_init();
    EnterCriticalSection(&lj_lock);
    fprintf(f, "[join]\nshown=%d\nplay_shown=%d\nscans=%d\nadapters=%d\ncandidates=%d\nrows=%d\nselected=%d\npaste_kind=%d\n"
               "busy=%d\noffer=%d\noffer_name=%s\nmessage=%s\nsteps=%d\nfind_lan=%d\nfind_tailscale=%d\nport=%d\nmigrated=%d\n",
            lj.shown, lj.play_shown, lj.scans, lj.adapters, lj.candidates, lj.list.n, lju.selected, lju.rec.kind, lju.busy,
            lju.offer, lju.offer_name, lju.message, lju.steps, lj.find_lan, lj.find_tailscale, lj.port, lju.migrated);
    fprintf(f, "recognised=%s\n", cr_describe(&lju.rec, line, sizeof(line)));
    for (i = 0; i < LJ_SRCS; i++) fprintf(f, "source%d=%d\nsource%d_found=%d\n", i, lj.src[i].state, i, lj.src[i].found);
    LeaveCriticalSection(&lj_lock);
    lj_primary_label(line, sizeof(line));
    fprintf(f, "primary=%s\n", line);
    fprintf(f, "rejoin=%s\n", lj_rejoin(line, sizeof(line), 0) ? line : "");
    EnterCriticalSection(&lj_lock);
    {
        int order[CD_ROWS], at[CD_ROWS];
        lj_order(&lj.list, order);
        for (i = 0; i < lj.list.n; i++) at[order[i]] = i;
        for (i = 0; i < lj.list.n; i++) {
            const struct cd_row *r = &lj.list.r[i];
            fprintf(f, "[row%d]\nname=%s\nversion=%s\nstate=%d\nplayers=%d\nlimit=%d\nroute=%s\nmismatch=%d\nip=%u.%u.%u.%u\nport=%u\n"
                       "rtt=%d\nshown_at=%d\n",
                    i, r->name, r->version, r->state, r->players, r->limit, cd_route_name(r->route), r->mismatch,
                    r->ip >> 24, r->ip >> 16 & 255, r->ip >> 8 & 255, r->ip & 255, r->port, r->rtt, at[i]);
        }
    }
    fprintf(f, "[recents]\ncount=%d\n", lj.recents.n);
    for (i = 0; i < lj.recents.n; i++) {
        const struct cd_recent *e = &lj.recents.r[i];
        fprintf(f, "[recent%d]\nlabel=%s\nname=%s\nhost=%s\nport=%u\nroute=%s\npinned=%d\nwhen=%llu\n", i, e->label, e->name, e->host,
                e->port, cd_recent_route_word(e->route), e->pinned, (unsigned long long)e->when);
    }
    LeaveCriticalSection(&lj_lock);
}
