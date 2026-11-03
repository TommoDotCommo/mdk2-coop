/* The Join side of the launcher, game-free: what a host answers (CPQ1 and CPQ2, rate limited), where
 * one Join-list scan sends its query, the sources line with its ticks, the row text, the order rows are
 * shown in, joining an invite by every route at once, the rejoin shortcut and the failure wording.
 * Portable C99 on top of coop_discover.h; the Windows side is launcher_join.c. See docs/DISCOVERY.md. */
#ifndef LAUNCHER_JOIN_H
#define LAUNCHER_JOIN_H
#include <stdio.h>
#include "coop_discover.h"

#define LJ_DEFAULT_PORT 27900u
#define LJ_PROBE_ID "MDK2"
#define LJ_GAME "MDK2"

/* ---- hosts answer (item 1): every listener that answered CPQ1 also answers CPQ2 ---- */
static inline int lj_is_probe(const uint8_t *q, size_t n)
{
    return (n == 8 && !memcmp(q, "CPQ1", 4)) || (n == CD_QUERY_LEN && !memcmp(q, "CPQ2", 4));
}
/* Reply length (0: stay silent). Probes from one source are rate limited (cd_rate_allow) before
 * cd_answer builds the reply; a v1 query still gets the v1 reply. */
static inline int lj_host_reply(struct cd_rate *rate, const uint8_t *q, size_t n, uint32_t from_ip, uint32_t now,
                                const struct cd_host *h, uint8_t *reply)
{
    if (!lj_is_probe(q, n)) return 0;
    if (!cd_rate_allow(rate, from_ip, now)) return 0;
    return cd_answer(q, n, h, reply);
}

/* ---- where one scan sends its query (B1, B2, B4) ---- */
/* The ports a scan covers: the port in Settings > Network and the default, each with the next three. */
static inline int lj_ports(unsigned configured, unsigned out[2])
{
    int n = 0;
    if (configured >= 1 && configured <= 65532) out[n++] = configured;
    if (!n || out[0] != LJ_DEFAULT_PORT) out[n++] = LJ_DEFAULT_PORT;
    return n;
}
struct lj_scan_input {
    const struct cd_adapter *adapters; int nadapters;
    const struct cd_ts *ts; int tailscale_on;
    const struct cd_recents *recents; const uint32_t *recent_ip;   /* resolved address per recent, 0 = numeric or unknown */
    unsigned ports[2]; int nports;
};
static inline int lj_candidates(struct cd_candidates *c, const struct lj_scan_input *in)
{
    int p, i;
    memset(c, 0, sizeof(*c));
    for (p = 0; p < in->nports; p++) {
        cd_add_adapters(c, in->adapters, in->nadapters, in->ports[p]);   /* every adapter's directed broadcast */
        if (in->tailscale_on && in->ts) cd_add_tailscale(c, in->ts, in->ports[p]);
    }
    for (i = 0; in->recents && i < in->recents->n; i++)
        cd_add_recent(c, &in->recents->r[i], in->recent_ip ? in->recent_ip[i] : 0);
    return c->n;
}

/* ---- B6: the sources, each named, with a tick when done ---- */
enum lj_src { LJ_SRC_LAN, LJ_SRC_TAILSCALE, LJ_SRC_RECENT, LJ_SRC_STEAM, LJ_SRCS };
enum lj_src_state { LJ_S_IDLE, LJ_S_LOOKING, LJ_S_DONE, LJ_S_OFF, LJ_S_MISSING, LJ_S_INVITES, LJ_S_FAILED };
struct lj_source { int state, found; };
static inline int lj_source_ticked(const struct lj_source *s) { return s->state == LJ_S_DONE; }
static inline const char *lj_source_text(int src, const struct lj_source *s, char *out, size_t cap)
{
    static const char *names[] = { "Network", "Tailscale", "Recent", "Steam" };
    const char *name = src >= 0 && src < LJ_SRCS ? names[src] : "";
    switch (s->state) {
    case LJ_S_LOOKING: snprintf(out, cap, "%s...", name); break;
    case LJ_S_DONE: snprintf(out, cap, "%s %d", name, s->found); break;
    case LJ_S_OFF: snprintf(out, cap, "%s off", name); break;
    case LJ_S_MISSING: snprintf(out, cap, "%s not installed", name); break;
    case LJ_S_INVITES: snprintf(out, cap, "%s: invites", name); break;
    case LJ_S_FAILED: snprintf(out, cap, "%s failed", name); break;
    default: snprintf(out, cap, "%s", name); break;
    }
    return out;
}

/* ---- B5: one row ---- */
static inline const char *lj_row_name(const struct cd_row *r) { return r->name[0] ? r->name : "Unnamed host"; }
static inline int lj_row_full(const struct cd_row *r) { return r->players >= r->limit; }
static inline const char *lj_state_text(const struct cd_row *r)
{
    if (lj_row_full(r)) return "full";
    return r->state == CD_IN_LOBBY ? "in lobby" : r->state == CD_IN_GAME ? "in game" : "reachable";
}
/* "in lobby  -  2 of 16  -  LAN  -  3 ms"; another version leads with it (shown greyed, never hidden). */
static inline int lj_row_line(const struct cd_row *r, char *out, size_t cap)
{
    int n;
    if (r->mismatch)
        n = snprintf(out, cap, "version %s  -  %s  -  %d of %d  -  %s  -  %d ms", r->version, lj_state_text(r), r->players,
                     r->limit, cd_route_name(r->route), r->rtt);
    else
        n = snprintf(out, cap, "%s  -  %d of %d  -  %s  -  %d ms", lj_state_text(r), r->players, r->limit,
                     cd_route_name(r->route), r->rtt);
    return n > 0 && (size_t)n < cap ? n : 0;
}
/* Shown order: this version first, then not full, then the better route, the shorter ping, the name. */
static inline int lj_row_before(const struct cd_row *a, const struct cd_row *b)
{
    if (a->mismatch != b->mismatch) return !a->mismatch;
    if (lj_row_full(a) != lj_row_full(b)) return !lj_row_full(a);
    if (a->route != b->route) return a->route < b->route;
    if (a->rtt != b->rtt) return a->rtt < b->rtt;
    return strcmp(a->name, b->name) < 0;
}
static inline int lj_order(const struct cd_list *l, int *idx)
{
    int i, j;
    for (i = 0; i < l->n; i++) {
        int v = i;
        for (j = i; j > 0 && lj_row_before(&l->r[v], &l->r[idx[j - 1]]); j--) idx[j] = idx[j - 1];
        idx[j] = v;
    }
    return l->n;
}

/* ---- F4: every failure names who and what and offers the next step ---- */
enum lj_fail { LJ_F_NONE, LJ_F_NO_ANSWER, LJ_F_VERSION, LJ_F_FULL, LJ_F_CODE, LJ_F_RESOLVE, LJ_F_NO_NETWORK, LJ_F_NOTHING };
#define LJ_N_TRY_AGAIN 1
#define LJ_N_UPDATE 2
#define LJ_N_DIAGNOSTICS 4
/* who: the host's name or address; what: the routes, the version or the code error. Returns the next steps. */
static inline int lj_failure(int kind, const char *who, const char *what, char *out, size_t cap)
{
    who = who && who[0] ? who : "the host";
    what = what ? what : "";
    switch (kind) {
    case LJ_F_NO_ANSWER:
        snprintf(out, cap, "No answer from %s (%s). Check that it is still hosting, then try again.", who, what);
        return LJ_N_TRY_AGAIN | LJ_N_DIAGNOSTICS;
    case LJ_F_VERSION:
        snprintf(out, cap, "%s has a different version (%s). Update to join.", who, what);
        return LJ_N_UPDATE | LJ_N_DIAGNOSTICS;
    case LJ_F_FULL:
        snprintf(out, cap, "%s's game is full (%s). Try again when someone leaves.", who, what);
        return LJ_N_TRY_AGAIN;
    case LJ_F_CODE:
        snprintf(out, cap, "%s", what);
        return LJ_N_TRY_AGAIN;
    case LJ_F_RESOLVE:
        snprintf(out, cap, "Could not find %s on your networks (%s).", who, what);
        return LJ_N_TRY_AGAIN | LJ_N_DIAGNOSTICS;
    case LJ_F_NO_NETWORK:
        snprintf(out, cap, "This PC has no network to look on (%s).", what);
        return LJ_N_TRY_AGAIN | LJ_N_DIAGNOSTICS;
    case LJ_F_NOTHING:
        snprintf(out, cap, "Pick a game from the list, or paste an invite or an address first.");
        return 0;
    default:
        if (cap) out[0] = 0;
        return 0;
    }
}

/* ---- C4: an invite's routes all at once; the first answer wins, a better one within 300 ms instead ---- */
struct lj_invite_join {
    struct cd_candidates c;
    struct cd_he he;
    int attempt_of[CD_MAX_CANDIDATES];  /* candidate -> attempt */
    int candidate_of[CD_HE_MAX];        /* attempt -> candidate */
};
static inline int lj_invite_start(struct lj_invite_join *j, const struct ci_invite *v)
{
    uint8_t routes[CD_HE_MAX];
    int i, n = 0;
    memset(j, 0, sizeof(*j));
    for (i = 0; i < v->n && n < CD_HE_MAX; i++) {
        int at = j->c.n;
        if (!cd_add(&j->c, cd_ip(v->route[i].ip), v->port, cd_route_of_kind(v->route[i].kind), 0, 0, 0)) continue;
        j->attempt_of[at] = n; j->candidate_of[n] = at; routes[n++] = j->c.c[at].route;
    }
    cd_he_start(&j->he, routes, n);
    return n;
}
static inline void lj_invite_reply(struct lj_invite_join *j, int candidate, uint32_t now)
{
    if (candidate >= 0 && candidate < j->c.n) cd_he_reply(&j->he, j->attempt_of[candidate], now);
}
/* The chosen candidate, or -1 while undecided (or when nothing answered). */
static inline int lj_invite_choice(struct lj_invite_join *j, uint32_t now)
{
    int a = cd_he_choice(&j->he, now);
    return a < 0 ? -1 : j->candidate_of[a];
}
/* Route words for the failure line: "LAN, Tailscale". */
static inline const char *lj_routes_text(const struct ci_invite *v, char *out, size_t cap)
{
    size_t used = 0;
    int i;
    if (cap) out[0] = 0;
    for (i = 0; i < v->n; i++) {
        int n = snprintf(out + used, cap - used, "%s%s", i ? ", " : "", cd_route_name(cd_route_of_kind(v->route[i].kind)));
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
    if (!v->n) snprintf(out, cap, "%s", v->flags & CI_FLAG_STEAM ? "Steam" : "no routes");
    return out;
}

/* ---- C5: the join page's "Open in launcher" starts the game with <mod>-coop://<CODE> on its command line ---- */
#define LJ_SCHEME "mdk2-coop://"
/* The link on a command line, copied into out (cap bytes): from the scheme to the next space or quote, the
 * scheme in lower case and without the trailing '/' a browser may add. Returns its length, 0 when there is
 * none (or it does not fit). */
static inline size_t lj_link_arg(const char *cmdline, char *out, size_t cap)
{
    size_t n = strlen(LJ_SCHEME), i, k;
    if (cap) out[0] = 0;
    for (i = 0; cmdline && cmdline[i]; i++) {
        for (k = 0; k < n && cmdline[i + k] && ci_upper((unsigned char)cmdline[i + k]) == ci_upper((unsigned char)LJ_SCHEME[k]); k++) {}
        if (k < n) continue;
        for (k = i; cmdline[k] && cmdline[k] != ' ' && cmdline[k] != '"' && cmdline[k] != '\t'; k++) {}
        if (k - i >= cap) return 0;
        memcpy(out, cmdline + i, k - i); out[k - i] = 0;
        for (k = 0; k < n; k++) out[k] = LJ_SCHEME[k];
        for (k = strlen(out); k > n && out[k - 1] == '/'; k--) out[k - 1] = 0;
        return k;
    }
    return 0;
}

/* ---- B4: recents ---- */
static inline int lj_recent_route(int route, int previous)
{
    switch (route) {
    case CD_LAN: return CD_R_LAN;
    case CD_TAILSCALE: return CD_R_TAILSCALE;
    case CD_STEAM: return CD_R_STEAM;
    case CD_INTERNET: return CD_R_INTERNET;
    default: return previous >= 0 && previous < CD_R_COUNT ? previous : CD_R_ADDRESS;
    }
}
/* First start: recents.tsv when present, else the saved addresses (profiles.tsv) become pinned recents.
 * Returns 1 when *out holds a store, with *migrated set when it came from the old store. */
static inline int lj_recents_load(struct cd_recents *out, const char *recents, size_t nrecents, int have_recents,
                                  const char *profiles, size_t nprofiles, int have_profiles, int *migrated)
{
    *migrated = 0;
    memset(out, 0, sizeof(*out));
    if (have_recents) return cd_recents_decode(out, recents, nrecents);
    if (have_profiles && cd_recents_decode(out, profiles, nprofiles)) { *migrated = 1; return 1; }
    return !have_profiles;
}
static inline const char *lj_recent_label(const struct cd_recent *e)
{
    return e->label[0] ? e->label : e->name[0] ? e->name : e->host;
}
/* A4: the last host joined (newest when), or -1. */
static inline int lj_last_recent(const struct cd_recents *s)
{
    int i, best = -1;
    for (i = 0; i < s->n; i++)
        if (s->r[i].when && (best < 0 || s->r[i].when > s->r[best].when)) best = i;
    return best;
}
/* The row that is this recent answering (same address on any route, same port), or -1. */
static inline int lj_row_of_recent(const struct cd_list *l, const struct cd_recent *e, uint32_t resolved)
{
    uint8_t ip[4];
    size_t used;
    int i, k;
    if (!resolved && cr_ipv4(e->host, strlen(e->host), ip, &used) && used == strlen(e->host)) resolved = cd_ip(ip);
    if (!resolved) return -1;
    for (i = 0; i < l->n; i++)
        for (k = 0; k < CD_ROUTES; k++)
            if (l->r[i].has[k] && l->r[i].ip_by[k] == resolved && l->r[i].port == e->port) return i;
    return -1;
}
/* "Rejoin Fox" when the last host answers with this version and room; else no shortcut (0). */
static inline int lj_rejoin_label(const struct cd_list *l, const struct cd_recents *s, uint32_t resolved, char *out, size_t cap)
{
    int last = lj_last_recent(s), row;
    if (last < 0 || (row = lj_row_of_recent(l, &s->r[last], resolved)) < 0) return 0;
    if (l->r[row].mismatch || lj_row_full(&l->r[row])) return 0;
    snprintf(out, cap, "Rejoin %s", l->r[row].name[0] ? l->r[row].name : lj_recent_label(&s->r[last]));
    return 1;
}
#endif
