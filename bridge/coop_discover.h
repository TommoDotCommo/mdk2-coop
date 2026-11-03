/* Finding co-op games: probe v2, candidate addresses, result rows, Tailscale status parser,
 * recent hosts store. Portable C99: no platform headers, no allocation, all input bounded.
 * Shared by MDK2 (bridge/) and The Witness (vendored copy); see docs/DISCOVERY.md. MIT licence.
 *
 * IPv4 addresses are uint32_t in host order (a.b.c.d = a<<24 | b<<16 | c<<8 | d).
 * Times are caller milliseconds (a wrapping tick counter is fine). */
#ifndef COOP_DISCOVER_H
#define COOP_DISCOVER_H
#include "coop_invite.h"

#define CD_PORTS 4                  /* default port and the next three */
#define CD_MAX_CANDIDATES 512
#define CD_ROWS 64
#define CD_TS_MAX 128
#define CD_REFRESH_MS 5000u
#define CD_WINDOW_MS 1200u          /* how long one scan listens for replies */
#define CD_STALE_MS 12000u          /* a route not heard for two refreshes and a window is dropped */
#define CD_PREFER_MS 300u           /* C4 preference window */
#define CD_RATE_PER_SOURCE 10       /* replies per second per source address */
#define CD_RATE_TOTAL 100           /* replies per second in total */
#define CD_RATE_SLOTS 32
#define CD_QUERY_LEN 13
#define CD_REPLY_MAX 97
#define CD_VERSION_MAX 47

/* Routes, best first (B5 order): LAN > Tailscale > Steam friend > Internet > Recent. */
enum cd_route { CD_LAN, CD_TAILSCALE, CD_STEAM, CD_INTERNET, CD_RECENT, CD_ROUTES };
enum cd_state { CD_IN_LOBBY, CD_IN_GAME, CD_REACHABLE };
#define CD_F_INVITE_ONLY 1u
#define CD_F_MISMATCH 2u

static inline const char *cd_route_name(int route)
{
    static const char *names[] = { "LAN", "Tailscale", "Steam friend", "Internet", "Recent" };
    return route >= 0 && route < CD_ROUTES ? names[route] : "";
}
static inline int cd_route_of_kind(int kind) { return kind == CI_LAN ? CD_LAN : kind == CI_VPN ? CD_TAILSCALE : CD_INTERNET; }
static inline uint32_t cd_ip(const uint8_t b[4]) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static inline uint32_t cd_le32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static inline uint32_t cd_mask(int prefix) { return prefix <= 0 ? 0 : prefix >= 32 ? 0xffffffffu : ~((1u << (32 - prefix)) - 1); }
static inline int cd_tailscale_ip(uint32_t ip) { return (ip & 0xffc00000u) == 0x64400000u; } /* 100.64.0.0/10 */

/* ---- probe v2 wire ---- */
/* Query: "CPQ2" + nonce u32 LE + game id (4 ASCII) + flags u8. */
static inline int cd_query(uint8_t *out, uint32_t nonce, const char *game_id, uint8_t flags)
{
    int k;
    if (strlen(game_id) != 4) return 0;
    memcpy(out, "CPQ2", 4);
    for (k = 0; k < 4; k++) out[4 + k] = (uint8_t)(nonce >> (8 * k));
    memcpy(out + 8, game_id, 4);
    out[12] = flags;
    return CD_QUERY_LEN;
}
struct cd_reply {
    uint32_t nonce;
    int state, players, limit, flags;
    char version[CD_VERSION_MAX + 1], name[CI_NAME_MAX + 1];
};
/* What a host says about itself. */
struct cd_host {
    const char *game_id, *version, *name;
    int state, invite_only, mismatch_possible;
    unsigned players, limit;
};
static inline int cd_version_ok(const uint8_t *s, size_t n)
{
    size_t i;
    if (!n || n > CD_VERSION_MAX) return 0;
    for (i = 0; i < n; i++) if (s[i] < 33 || s[i] > 126) return 0;
    return 1;
}
/* Strict parse of "CPS2" + nonce + game id + state + players + limit + flags + version len +
 * version + name len + name. Accepts only the expected nonce and game id. */
static inline int cd_parse_reply(const uint8_t *b, size_t n, uint32_t nonce, const char *game_id, struct cd_reply *out)
{
    struct cd_reply t;
    size_t vlen, nlen;
    char clean[CI_NAME_MAX + 1];
    if (n < 19 || n > CD_REPLY_MAX || memcmp(b, "CPS2", 4)) return 0;
    if (cd_le32(b + 4) != nonce || memcmp(b + 8, game_id, 4)) return 0;
    vlen = b[16];
    if (!cd_version_ok(b + 17, vlen) || 17 + vlen + 1 > n) return 0;
    nlen = b[17 + vlen];
    if (nlen > CI_NAME_MAX || n != 18 + vlen + nlen) return 0;
    memset(&t, 0, sizeof(t));
    t.nonce = nonce; t.state = b[12]; t.players = b[13]; t.limit = b[14]; t.flags = b[15];
    if (t.state > CD_REACHABLE || !t.limit || t.players > t.limit) return 0;
    if ((t.flags & CD_F_INVITE_ONLY) && nlen) return 0;
    memcpy(t.version, b + 17, vlen);
    if (ci_name_clean((const char *)b + 18 + vlen, nlen, clean) != nlen) return 0;
    memcpy(t.name, clean, nlen + 1);
    *out = t;
    return 1;
}
/* Host side: reply to CPQ2 (same game id only) and keep answering CPQ1 as v1 did.
 * Returns the reply length, or 0 to stay silent. reply needs CD_REPLY_MAX bytes. */
static inline int cd_answer(const uint8_t *q, size_t n, const struct cd_host *h, uint8_t *reply)
{
    size_t vlen = strlen(h->version), nlen;
    char clean[CI_NAME_MAX + 1];
    if (!cd_version_ok((const uint8_t *)h->version, vlen) || h->state < 0 || h->state > CD_REACHABLE ||
        !h->limit || h->limit > 255 || h->players > h->limit) return 0;
    if (n == 8 && !memcmp(q, "CPQ1", 4)) {
        unsigned limit = h->limit > 16 ? 16 : h->limit, players = h->players > limit ? limit : h->players;
        memcpy(reply, "CPS1", 4); memcpy(reply + 4, q + 4, 4);
        reply[8] = (uint8_t)players; reply[9] = (uint8_t)limit; reply[10] = (uint8_t)h->state; reply[11] = (uint8_t)vlen;
        memcpy(reply + 12, h->version, vlen);
        return (int)(12 + vlen);
    }
    if (n != CD_QUERY_LEN || memcmp(q, "CPQ2", 4) || memcmp(q + 8, h->game_id, 4)) return 0;
    nlen = h->invite_only || !h->name ? 0 : ci_name_clean(h->name, strlen(h->name), clean);
    memcpy(reply, "CPS2", 4); memcpy(reply + 4, q + 4, 8);
    reply[12] = (uint8_t)h->state; reply[13] = (uint8_t)h->players; reply[14] = (uint8_t)h->limit;
    reply[15] = (uint8_t)((h->invite_only ? CD_F_INVITE_ONLY : 0) | (h->mismatch_possible ? CD_F_MISMATCH : 0));
    reply[16] = (uint8_t)vlen; memcpy(reply + 17, h->version, vlen);
    reply[17 + vlen] = (uint8_t)nlen; memcpy(reply + 18 + vlen, clean, nlen);
    return (int)(18 + vlen + nlen);
}

/* Per-source reply rate limit (token buckets) with a total cap, so a spoofed flood cannot
 * turn a host into an amplifier. Zero-initialise. */
struct cd_rate {
    struct { uint32_t ip, last; int tokens, used; } s[CD_RATE_SLOTS];
    uint32_t total_last;
    int total_tokens, total_used;
};
static inline int cd_bucket(uint32_t *last, int *tokens, int *used, int burst, uint32_t now)
{
    uint32_t step = 1000u / (uint32_t)burst, add;
    if (!*used) { *used = 1; *tokens = burst; *last = now; }
    add = (uint32_t)(now - *last) / step;
    if (add) { *tokens = add >= (uint32_t)burst ? burst : (*tokens + (int)add > burst ? burst : *tokens + (int)add); *last += add * step; }
    if (*tokens <= 0) return 0;
    (*tokens)--;
    return 1;
}
static inline int cd_rate_allow(struct cd_rate *r, uint32_t ip, uint32_t now)
{
    int i, slot = -1, oldest = 0;
    for (i = 0; i < CD_RATE_SLOTS; i++) {
        if (r->s[i].used && r->s[i].ip == ip) { slot = i; break; }
        if (!r->s[i].used || (r->s[oldest].used && (int32_t)(r->s[i].last - r->s[oldest].last) < 0)) oldest = i;
    }
    if (slot < 0) { slot = oldest; r->s[slot].used = 0; r->s[slot].ip = ip; }
    if (!cd_bucket(&r->s[slot].last, &r->s[slot].tokens, &r->s[slot].used, CD_RATE_PER_SOURCE, now)) return 0;
    return cd_bucket(&r->total_last, &r->total_tokens, &r->total_used, CD_RATE_TOTAL, now);
}

/* ---- candidates: where one scan sends its query ---- */
struct cd_adapter { uint32_t ip; int prefix; };
struct cd_candidate { uint32_t ip, mask; uint16_t port; uint8_t route, broadcast; int group; };
struct cd_candidates { int n, dropped; struct cd_candidate c[CD_MAX_CANDIDATES]; };
static inline int cd_add(struct cd_candidates *s, uint32_t ip, unsigned port, int route, int broadcast, uint32_t mask, int group)
{
    int i;
    if (!ip || !port || port > 65535) return 0;
    for (i = 0; i < s->n; i++) if (s->c[i].ip == ip && s->c[i].port == port) return 0;
    if (s->n >= CD_MAX_CANDIDATES) { s->dropped++; return 0; }
    s->c[s->n].ip = ip; s->c[s->n].mask = mask; s->c[s->n].port = (uint16_t)port;
    s->c[s->n].route = (uint8_t)route; s->c[s->n].broadcast = (uint8_t)broadcast; s->c[s->n].group = group;
    s->n++;
    return 1;
}
/* B1: the directed broadcast address of EVERY up IPv4 adapter, default port and the next three.
 * Loopback, Tailscale's own adapter (peers are probed directly) and point-to-point /31 /32 links
 * have no broadcast to use. */
static inline int cd_add_adapters(struct cd_candidates *s, const struct cd_adapter *a, int n, unsigned port)
{
    int i, k, added = 0;
    for (i = 0; i < n; i++) {
        uint32_t mask = cd_mask(a[i].prefix), broadcast = a[i].ip | ~mask;
        if (a[i].prefix < 8 || a[i].prefix > 30 || (a[i].ip >> 24) == 127 || !a[i].ip || cd_tailscale_ip(a[i].ip)) continue;
        for (k = 0; k < CD_PORTS; k++) added += cd_add(s, broadcast, port + (unsigned)k, CD_LAN, 1, mask, -1);
    }
    return added;
}
static inline int cd_add_invite(struct cd_candidates *s, const struct ci_invite *v, int group)
{
    int i, added = 0;
    for (i = 0; i < v->n; i++) added += cd_add(s, cd_ip(v->route[i].ip), v->port, cd_route_of_kind(v->route[i].kind), 0, 0, group);
    return added;
}
/* Which candidate a reply belongs to. A reply to a broadcast comes from the host's own address,
 * so it matches a broadcast candidate whose subnet holds it. Several matches: the best route.
 * Returns the candidate index, or -1 for a source nobody asked. */
static inline int cd_match(const struct cd_candidates *s, uint32_t ip, unsigned port)
{
    int i, best = -1;
    for (i = 0; i < s->n; i++) {
        const struct cd_candidate *c = &s->c[i];
        if (c->port != port) continue;
        if (c->broadcast ? (ip & c->mask) == (c->ip & c->mask) && ip != c->ip : c->ip == ip)
            if (best < 0 || c->route < s->c[best].route) best = i;
    }
    return best;
}

/* ---- B2: `tailscale status --json`, minimal and bounded ---- */
struct cd_ts_peer { uint32_t ip; int online, sharee; char host[64], dns[128], os[16]; };
struct cd_ts {
    int running, n, total, offline, no_ipv4;
    struct cd_ts_peer self;
    struct cd_ts_peer peer[CD_TS_MAX];
};
#define CD_JSON_MAX (4u << 20)
#define CD_JSON_DEPTH 32
struct cd_js { const char *s; size_t n, i; int depth; };
static inline void cd_js_ws(struct cd_js *j) { while (j->i < j->n && ci_space((unsigned char)j->s[j->i])) j->i++; }
static inline int cd_js_lit(struct cd_js *j, const char *word)
{
    size_t n = strlen(word);
    if (j->n - j->i < n || memcmp(j->s + j->i, word, n)) return 0;
    j->i += n;
    return 1;
}
static inline int cd_js_hex(struct cd_js *j, uint32_t *v)
{
    int k;
    *v = 0;
    if (j->n - j->i < 4) return 0;
    for (k = 0; k < 4; k++) {
        int c = (unsigned char)j->s[j->i++], d = c >= '0' && c <= '9' ? c - '0' : ci_upper(c) >= 'A' && ci_upper(c) <= 'F' ? ci_upper(c) - 'A' + 10 : -1;
        if (d < 0) return 0;
        *v = *v << 4 | (uint32_t)d;
    }
    return 1;
}
/* A string into out (cap bytes, truncated on a character boundary; NULL to skip). */
static inline int cd_js_string(struct cd_js *j, char *out, size_t cap)
{
    size_t used = 0;
    int cut = 0;
    cd_js_ws(j);
    if (j->i >= j->n || j->s[j->i] != '"') return 0;
    j->i++;
    while (j->i < j->n) {
        unsigned char c = (unsigned char)j->s[j->i++];
        uint8_t enc[4];
        size_t len = 1;
        if (c == '"') { if (out && cap) out[used] = 0; return 1; }
        if (c < 0x20) return 0;
        enc[0] = c;
        if (c == '\\') {
            uint32_t cp;
            if (j->i >= j->n) return 0;
            c = (unsigned char)j->s[j->i++];
            if (c == 'u') {
                if (!cd_js_hex(j, &cp)) return 0;
                if (cp >= 0xd800 && cp < 0xdc00) {
                    uint32_t lo;
                    if (!cd_js_lit(j, "\\u") || !cd_js_hex(j, &lo) || lo < 0xdc00 || lo > 0xdfff) return 0;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff) return 0;
                if (cp < 0x80) enc[0] = (uint8_t)cp;
                else if (cp < 0x800) { enc[0] = (uint8_t)(0xc0 | cp >> 6); enc[1] = (uint8_t)(0x80 | (cp & 63)); len = 2; }
                else if (cp < 0x10000) { enc[0] = (uint8_t)(0xe0 | cp >> 12); enc[1] = (uint8_t)(0x80 | (cp >> 6 & 63)); enc[2] = (uint8_t)(0x80 | (cp & 63)); len = 3; }
                else { enc[0] = (uint8_t)(0xf0 | cp >> 18); enc[1] = (uint8_t)(0x80 | (cp >> 12 & 63)); enc[2] = (uint8_t)(0x80 | (cp >> 6 & 63)); enc[3] = (uint8_t)(0x80 | (cp & 63)); len = 4; }
            } else {
                const char *from = "\"\\/bfnrt", *to = "\"\\/\b\f\n\r\t", *p = strchr(from, c);
                if (!c || !p) return 0;
                enc[0] = (uint8_t)to[p - from];
            }
        } else if (c >= 0x80) {
            /* copy a raw UTF-8 sequence whole so truncation never splits it */
            len = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
            if (!len || j->n - j->i < len - 1) return 0;
            memcpy(enc + 1, j->s + j->i, len - 1); j->i += len - 1;
        }
        if (out && !cut) { if (used + len < cap) { memcpy(out + used, enc, len); used += len; } else cut = 1; }
    }
    return 0;
}
static inline int cd_js_skip(struct cd_js *j)
{
    char c;
    cd_js_ws(j);
    if (j->i >= j->n) return 0;
    c = j->s[j->i];
    if (c == '"') return cd_js_string(j, NULL, 0);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        if (++j->depth > CD_JSON_DEPTH) return 0;
        j->i++; cd_js_ws(j);
        if (j->i < j->n && j->s[j->i] == close) { j->i++; j->depth--; return 1; }
        for (;;) {
            if (c == '{') { if (!cd_js_string(j, NULL, 0)) return 0; cd_js_ws(j); if (j->i >= j->n || j->s[j->i++] != ':') return 0; }
            if (!cd_js_skip(j)) return 0;
            cd_js_ws(j);
            if (j->i >= j->n) return 0;
            if (j->s[j->i] == ',') { j->i++; continue; }
            if (j->s[j->i] == close) { j->i++; j->depth--; return 1; }
            return 0;
        }
    }
    if (cd_js_lit(j, "true") || cd_js_lit(j, "false") || cd_js_lit(j, "null")) return 1;
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = j->i;
        while (j->i < j->n && strchr("+-.eE0123456789", j->s[j->i]) && j->s[j->i]) j->i++;
        return j->i > start;
    }
    return 0;
}
static inline int cd_js_bool(struct cd_js *j, int *v)
{
    cd_js_ws(j);
    if (cd_js_lit(j, "true")) { *v = 1; return 1; }
    if (cd_js_lit(j, "false") || cd_js_lit(j, "null")) { *v = 0; return 1; }
    return 0;
}
/* Iterate an object: calls field(j, key, ctx) for each key; field must consume the value. */
static inline int cd_js_object(struct cd_js *j, int (*field)(struct cd_js *, const char *, void *), void *ctx)
{
    cd_js_ws(j);
    if (cd_js_lit(j, "null")) return 1;
    if (j->i >= j->n || j->s[j->i] != '{' || ++j->depth > CD_JSON_DEPTH) return 0;
    j->i++; cd_js_ws(j);
    if (j->i < j->n && j->s[j->i] == '}') { j->i++; j->depth--; return 1; }
    for (;;) {
        char key[64];
        if (!cd_js_string(j, key, sizeof(key))) return 0;
        cd_js_ws(j);
        if (j->i >= j->n || j->s[j->i++] != ':' || !field(j, key, ctx)) return 0;
        cd_js_ws(j);
        if (j->i >= j->n) return 0;
        if (j->s[j->i] == ',') { j->i++; continue; }
        if (j->s[j->i] == '}') { j->i++; j->depth--; return 1; }
        return 0;
    }
}
static inline int cd_ts_peer_field(struct cd_js *j, const char *key, void *ctx)
{
    struct cd_ts_peer *p = (struct cd_ts_peer *)ctx;
    if (!strcmp(key, "HostName")) return cd_js_string(j, p->host, sizeof(p->host));
    if (!strcmp(key, "DNSName")) return cd_js_string(j, p->dns, sizeof(p->dns));
    if (!strcmp(key, "OS")) return cd_js_string(j, p->os, sizeof(p->os));
    if (!strcmp(key, "Online")) return cd_js_bool(j, &p->online);
    if (!strcmp(key, "ShareeNode")) return cd_js_bool(j, &p->sharee);
    if (!strcmp(key, "TailscaleIPs")) {
        cd_js_ws(j);
        if (cd_js_lit(j, "null")) return 1;
        if (j->i >= j->n || j->s[j->i] != '[') return 0;
        j->i++; cd_js_ws(j);
        if (j->i < j->n && j->s[j->i] == ']') { j->i++; return 1; }
        for (;;) {
            char text[64];
            uint8_t ip[4];
            size_t used;
            if (!cd_js_string(j, text, sizeof(text))) return 0;
            if (!p->ip && cr_ipv4(text, strlen(text), ip, &used) && used == strlen(text)) p->ip = cd_ip(ip);
            cd_js_ws(j);
            if (j->i >= j->n) return 0;
            if (j->s[j->i] == ',') { j->i++; continue; }
            if (j->s[j->i] == ']') { j->i++; return 1; }
            return 0;
        }
    }
    return cd_js_skip(j);
}
static inline int cd_ts_peers_field(struct cd_js *j, const char *key, void *ctx)
{
    struct cd_ts *t = (struct cd_ts *)ctx;
    struct cd_ts_peer p;
    memset(&p, 0, sizeof(p));
    (void)key;
    if (!cd_js_object(j, cd_ts_peer_field, &p)) return 0;
    t->total++;
    if (!p.online) t->offline++;
    else if (!p.ip) t->no_ipv4++;
    else if (t->n < CD_TS_MAX) t->peer[t->n++] = p;
    return 1;
}
static inline int cd_ts_top_field(struct cd_js *j, const char *key, void *ctx)
{
    struct cd_ts *t = (struct cd_ts *)ctx;
    if (!strcmp(key, "BackendState")) {
        char state[32];
        if (!cd_js_string(j, state, sizeof(state))) return 0;
        t->running = !strcmp(state, "Running");
        return 1;
    }
    if (!strcmp(key, "Self")) return cd_js_object(j, cd_ts_peer_field, &t->self);
    if (!strcmp(key, "Peer")) return cd_js_object(j, cd_ts_peers_field, t);
    return cd_js_skip(j);
}
/* Online peers with an IPv4 Tailscale address (shared-in nodes included). Returns 1, or 0 with
 * *t cleared for malformed or truncated text. Peers are only listed when the backend is Running. */
static inline int cd_ts_parse(const char *json, size_t len, struct cd_ts *t)
{
    struct cd_js j;
    size_t n;
    memset(t, 0, sizeof(*t));
    j.s = json; j.n = len; j.i = 0; j.depth = 0;
    if (len > CD_JSON_MAX || !cd_js_object(&j, cd_ts_top_field, t)) { memset(t, 0, sizeof(*t)); return 0; }
    cd_js_ws(&j);
    if (j.i != j.n) { memset(t, 0, sizeof(*t)); return 0; }
    if (!t->running) t->n = 0;
    n = strlen(t->self.dns);
    if (n && t->self.dns[n - 1] == '.') t->self.dns[n - 1] = 0;
    return 1;
}
/* B2: each online peer's IPv4 Tailscale address, default port and the next three. */
static inline int cd_add_tailscale(struct cd_candidates *s, const struct cd_ts *t, unsigned port)
{
    int i, k, added = 0;
    for (i = 0; i < t->n; i++)
        if (t->running && t->peer[i].online && t->peer[i].ip)
            for (k = 0; k < CD_PORTS; k++) added += cd_add(s, t->peer[i].ip, port + (unsigned)k, CD_TAILSCALE, 0, 0, -1);
    return added;
}

/* ---- rows: one per host however many routes reached it ---- */
struct cd_row {
    char name[CI_NAME_MAX + 1], version[CD_VERSION_MAX + 1];
    int state, players, limit, flags, mismatch, group, route, rtt;
    uint32_t ip; uint16_t port;
    uint32_t seen[CD_ROUTES], ip_by[CD_ROUTES];
    int rtt_by[CD_ROUTES], has[CD_ROUTES];
};
struct cd_list { int n; struct cd_row r[CD_ROWS]; };
static inline void cd_row_best(struct cd_row *row)
{
    int k;
    for (k = 0; k < CD_ROUTES && !row->has[k]; k++) {}
    row->route = k;
    if (k < CD_ROUTES) { row->ip = row->ip_by[k]; row->rtt = row->rtt_by[k]; }
}
/* Merge one sighting. Same host = same invite group, or the same non-empty name, version and
 * port. Invite-only hosts are listed only for an invite (B9). Returns the row index or -1. */
static inline int cd_seen(struct cd_list *l, const struct cd_reply *rep, int route, uint32_t ip, unsigned port, int group,
                          int rtt, uint32_t now, const char *our_version)
{
    int i;
    struct cd_row *row = NULL;
    if (route < 0 || route >= CD_ROUTES) return -1;
    if ((rep->flags & CD_F_INVITE_ONLY) && group < 0) return -1;
    for (i = 0; i < l->n && !row; i++) {
        struct cd_row *r = &l->r[i];
        if (group >= 0 ? r->group == group
                       : r->group < 0 && (rep->name[0] ? !strcmp(r->name, rep->name) && !strcmp(r->version, rep->version) && r->port == port
                                                       : !r->name[0] && r->ip == ip && r->port == port)) row = r;
    }
    if (!row) {
        if (l->n >= CD_ROWS) return -1;
        row = &l->r[l->n++];
        memset(row, 0, sizeof(*row));
        row->group = group;
    }
    memcpy(row->name, rep->name, sizeof(row->name)); memcpy(row->version, rep->version, sizeof(row->version));
    row->state = rep->state; row->players = rep->players; row->limit = rep->limit; row->flags = rep->flags;
    row->mismatch = our_version && strcmp(rep->version, our_version);
    row->port = (uint16_t)port;
    row->has[route] = 1; row->seen[route] = now; row->ip_by[route] = ip; row->rtt_by[route] = rtt;
    cd_row_best(row);
    return (int)(row - l->r);
}
/* Drop routes not heard for CD_STALE_MS and rows with no route left. */
static inline void cd_expire(struct cd_list *l, uint32_t now)
{
    int i, k, out = 0;
    for (i = 0; i < l->n; i++) {
        int any = 0;
        for (k = 0; k < CD_ROUTES; k++) {
            if (l->r[i].has[k] && (uint32_t)(now - l->r[i].seen[k]) > CD_STALE_MS) l->r[i].has[k] = 0;
            any |= l->r[i].has[k];
        }
        if (!any) continue;
        cd_row_best(&l->r[i]);
        if (out != i) l->r[out] = l->r[i];
        out++;
    }
    l->n = out;
}
/* B6: scan as soon as the list is shown, then every 5 s while it stays shown; nothing when hidden. */
struct cd_refresh { uint32_t last; int ever; };
static inline int cd_refresh_due(struct cd_refresh *f, int shown, uint32_t now)
{
    if (!shown) { f->ever = 0; return 0; }
    if (f->ever && (uint32_t)(now - f->last) < CD_REFRESH_MS) return 0;
    f->ever = 1; f->last = now;
    return 1;
}

/* ---- C4: try every route of an invite at once ---- */
#define CD_HE_MAX 8
struct cd_he { int n, first; uint32_t first_at; uint8_t route[CD_HE_MAX], replied[CD_HE_MAX]; };
static inline void cd_he_start(struct cd_he *h, const uint8_t *routes, int n)
{
    memset(h, 0, sizeof(*h));
    h->n = n > CD_HE_MAX ? CD_HE_MAX : n; h->first = -1;
    memcpy(h->route, routes, (size_t)h->n);
}
static inline void cd_he_reply(struct cd_he *h, int index, uint32_t now)
{
    if (index < 0 || index >= h->n || h->replied[index]) return;
    h->replied[index] = 1;
    if (h->first < 0) { h->first = index; h->first_at = now; }
}
/* -1 while waiting; else the chosen attempt: the first answer, or a better route that answered
 * within CD_PREFER_MS of it. Decides early once the best route tried has answered. */
static inline int cd_he_choice(struct cd_he *h, uint32_t now)
{
    int i, best = -1, top = CD_ROUTES;
    if (h->first < 0) return -1;
    for (i = 0; i < h->n; i++) {
        if (h->route[i] < top) top = h->route[i];
        if (h->replied[i] && (best < 0 || h->route[i] < h->route[best])) best = i;
    }
    if (h->route[best] == top || (uint32_t)(now - h->first_at) >= CD_PREFER_MS) return best;
    return -1;
}

/* ---- B4: recent hosts, a versioned text store; replaces MDK2's saved addresses ---- */
#define CD_RECENT_MAX 32
#define CD_RECENT_BYTES 16384
enum cd_recent_route { CD_R_LAN, CD_R_TAILSCALE, CD_R_STEAM, CD_R_INTERNET, CD_R_ADDRESS, CD_R_COUNT };
struct cd_recent { char label[49], name[CI_NAME_MAX + 1], host[128], notes[161]; unsigned port; int route, pinned; uint64_t when; };
struct cd_recents { int n; struct cd_recent r[CD_RECENT_MAX]; };
static inline const char *cd_recent_route_word(int route)
{
    static const char *words[] = { "lan", "tailscale", "steam", "internet", "address" };
    return route >= 0 && route < CD_R_COUNT ? words[route] : NULL;
}
/* Printable UTF-8 text shorter than max bytes. */
static inline int cd_text_ok(const char *s, size_t max, int empty)
{
    size_t n = strlen(s), i = 0, len;
    if (n >= max || (!empty && !n)) return 0;
    while (i < n) { if (!(len = ci_char(s, n, i))) return 0; i += len; }
    return 1;
}
static inline int cd_recent_valid(const struct cd_recent *e)
{
    size_t i;
    char clean[CI_NAME_MAX + 1];
    if (!cd_text_ok(e->label, sizeof(e->label), 1) || !cd_text_ok(e->notes, sizeof(e->notes), 1) ||
        ci_name_clean(e->name, strlen(e->name), clean) != strlen(e->name) || strlen(e->host) >= sizeof(e->host) || !e->host[0] ||
        e->port < 1 || e->port > 65535 || e->route < 0 || e->route >= CD_R_COUNT || (e->pinned != 0 && e->pinned != 1)) return 0;
    for (i = 0; e->host[i]; i++)
        if (!ci_alnum((unsigned char)e->host[i]) && e->host[i] != '.' && e->host[i] != '-') return 0;
    return 1;
}
static inline int cd_same_host(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (ci_upper((unsigned char)*a) != ci_upper((unsigned char)*b)) return 0;
    return !*a && !*b;
}
/* Transactional decode: COOP-RECENTS v1, or a COOP-PROFILES v0/v1 store from the saved addresses
 * page, whose rows become pinned recents (label = saved name, route = typed address). */
static inline int cd_recents_decode(struct cd_recents *out, const char *data, size_t size)
{
    struct cd_recents t;
    char buffer[CD_RECENT_BYTES], *line, *next;
    int format, fields;
    memset(&t, 0, sizeof(t));
    if (size >= sizeof(buffer) || memchr(data, 0, size)) return 0;
    memcpy(buffer, data, size); buffer[size] = 0;
    if (!strncmp(buffer, "COOP-RECENTS\t1\n", 15)) { format = 2; fields = 8; line = buffer + 15; }
    else if (!strncmp(buffer, "COOP-PROFILES\t1\n", 16)) { format = 1; fields = 5; line = buffer + 16; }
    else if (!strncmp(buffer, "COOP-PROFILES\t0\n", 16)) { format = 0; fields = 3; line = buffer + 16; }
    else return 0;
    while (*line) {
        char *f[8], *p;
        int n = 1, j, route = -1;
        uint64_t port, when = 0, pinned = 1;
        struct cd_recent *e;
        next = strchr(line, '\n');
        if (!next || t.n >= CD_RECENT_MAX) return 0;
        *next++ = 0;
        f[0] = line;
        for (p = line; *p; p++) if (*p == '\t') { if (n >= 8) return 0; *p = 0; f[n++] = p + 1; }
        if (n != fields) return 0;
        e = &t.r[t.n];
        if (format == 2) {
            for (j = 0; j < CD_R_COUNT; j++) if (!strcmp(f[4], cd_recent_route_word(j))) route = j;
            if (route < 0 || !cr_digits(f[3], strlen(f[3]), 65535, &port) || !cr_digits(f[5], strlen(f[5]), UINT64_MAX, &when) ||
                !cr_digits(f[6], strlen(f[6]), 1, &pinned) || strlen(f[0]) >= sizeof(e->label) || strlen(f[1]) >= sizeof(e->name) ||
                strlen(f[2]) >= sizeof(e->host) || strlen(f[7]) >= sizeof(e->notes)) return 0;
            strcpy(e->label, f[0]); strcpy(e->name, f[1]); strcpy(e->host, f[2]); strcpy(e->notes, f[7]);
        } else {
            route = CD_R_ADDRESS;
            if (!cr_digits(f[2], strlen(f[2]), 65535, &port) || (format && !cr_digits(f[4], strlen(f[4]), UINT64_MAX, &when)) ||
                !f[0][0] || strlen(f[0]) >= sizeof(e->label) || strlen(f[1]) >= sizeof(e->host) ||
                (format && strlen(f[3]) >= sizeof(e->notes))) return 0;
            strcpy(e->label, f[0]); strcpy(e->host, f[1]);
            if (format) strcpy(e->notes, f[3]);
        }
        e->port = (unsigned)port; e->route = route; e->when = when; e->pinned = (int)pinned;
        if (!cd_recent_valid(e)) return 0;
        for (j = 0; j < t.n; j++) if (cd_same_host(t.r[j].host, e->host) && t.r[j].port == e->port) return 0;
        t.n++; line = next;
    }
    *out = t;
    return 1;
}
static inline int cd_recents_encode(const struct cd_recents *s, char *out, size_t capacity)
{
    size_t used = 15;
    int i, j, n;
    if (s->n < 0 || s->n > CD_RECENT_MAX || capacity < 16) return 0;
    memcpy(out, "COOP-RECENTS\t1\n", 16);
    for (i = 0; i < s->n; i++) {
        const struct cd_recent *e = &s->r[i];
        if (!cd_recent_valid(e)) return 0;
        for (j = 0; j < i; j++) if (cd_same_host(s->r[j].host, e->host) && s->r[j].port == e->port) return 0;
        n = snprintf(out + used, capacity - used, "%s\t%s\t%s\t%u\t%s\t%llu\t%d\t%s\n", e->label, e->name, e->host, e->port,
                     cd_recent_route_word(e->route), (unsigned long long)e->when, e->pinned, e->notes);
        if (n < 0 || (size_t)n >= capacity - used) return 0;
        used += (size_t)n;
    }
    return (int)used;
}
/* After a successful join: refresh the entry for host:port, or add one, evicting the oldest
 * unpinned entry when full. Returns the index, or -1 (invalid, or everything pinned). */
static inline int cd_recent_add(struct cd_recents *s, const char *name, const char *host, unsigned port, int route, uint64_t now)
{
    struct cd_recent e;
    int i, oldest = -1;
    memset(&e, 0, sizeof(e));
    if (strlen(host) >= sizeof(e.host)) return -1;
    ci_name_clean(name, strlen(name), e.name);
    strcpy(e.host, host); e.port = port; e.route = route; e.when = now;
    if (!cd_recent_valid(&e)) return -1;
    for (i = 0; i < s->n; i++) {
        if (cd_same_host(s->r[i].host, host) && s->r[i].port == port) {
            strcpy(s->r[i].name, e.name); s->r[i].route = route; s->r[i].when = now;
            return i;
        }
        if (!s->r[i].pinned && (oldest < 0 || s->r[i].when < s->r[oldest].when)) oldest = i;
    }
    if (s->n < CD_RECENT_MAX) { s->r[s->n] = e; return s->n++; }
    if (oldest < 0) return -1;
    memmove(&s->r[oldest], &s->r[oldest + 1], (size_t)(s->n - oldest - 1) * sizeof(s->r[0]));
    s->r[s->n - 1] = e;
    return s->n - 1;
}
static inline int cd_recent_forget(struct cd_recents *s, int i)
{
    if (i < 0 || i >= s->n) return 0;
    memmove(&s->r[i], &s->r[i + 1], (size_t)(s->n - i - 1) * sizeof(s->r[0]));
    s->n--;
    return 1;
}
static inline int cd_recent_rename(struct cd_recents *s, int i, const char *label)
{
    if (i < 0 || i >= s->n || strlen(label) >= sizeof(s->r[i].label) || !cd_text_ok(label, sizeof(s->r[i].label), 1)) return 0;
    strcpy(s->r[i].label, label);
    return 1;
}
/* Probe a recent whose host is a numeric IPv4 address (names resolve in the adapter first). */
static inline int cd_add_recent(struct cd_candidates *s, const struct cd_recent *e, uint32_t resolved)
{
    uint8_t ip[4];
    size_t used;
    if (!resolved && cr_ipv4(e->host, strlen(e->host), ip, &used) && used == strlen(e->host)) resolved = cd_ip(ip);
    return resolved ? cd_add(s, resolved, e->port, CD_RECENT, 0, 0, -1) : 0;
}
#endif
