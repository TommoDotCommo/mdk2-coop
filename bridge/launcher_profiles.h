/* Portable profile store and probe policy. Addresses stay in the local store and
 * are sent only to the selected endpoint; no directory service or telemetry. */
#ifndef LAUNCHER_PROFILES_H
#define LAUNCHER_PROFILES_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#define LP_MAX 32
#define LP_BYTES 16384
struct lp_profile { char name[49], host[128], notes[161]; unsigned port; uint64_t used; };
struct lp_store { int n; struct lp_profile p[LP_MAX]; };
enum lp_status { LP_UNKNOWN, LP_OFFLINE, LP_REACHABLE, LP_LOBBY, LP_GAME, LP_MISMATCH, LP_FULL };
struct lp_probe { uint32_t nonce, sent, next, address; int pending, status, rtt; char version[48]; };
static inline int lp_text(const char *s, size_t max, int empty)
{
    size_t n = strlen(s), i;
    if (n >= max || (!empty && !n)) return 0;
    for (i = 0; i < n; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return 0;
    return 1;
}
static inline int lp_valid(const struct lp_profile *p)
{
    size_t i;
    if (!lp_text(p->name, sizeof(p->name), 0) || !lp_text(p->notes, sizeof(p->notes), 1) ||
        !lp_text(p->host, sizeof(p->host), 0) || p->port < 1 || p->port > 65535) return 0;
    for (i = 0; p->host[i]; i++)
        if (!isalnum((unsigned char)p->host[i]) && p->host[i] != '.' && p->host[i] != '-') return 0;
    return 1;
}
static inline int lp_number(const char *s, uint64_t *v)
{
    uint64_t n = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        unsigned d = (unsigned char)*s - '0';
        if (d > 9 || n > (UINT64_MAX - d) / 10) return 0;
        n = n * 10 + d;
    }
    *v = n; return 1;
}
/* Transactional decode: invalid input never changes the caller's store. v0
 * migration accepts name/host/port rows; v1 also stores notes and last-used. */
static inline int lp_decode(struct lp_store *out, const char *data, size_t size)
{
    struct lp_store temp = {0};
    char buffer[LP_BYTES], *line, *next;
    int version;
    if (size >= sizeof(buffer) || memchr(data, 0, size)) return 0;
    memcpy(buffer, data, size); buffer[size] = 0;
    if (!strncmp(buffer, "COOP-PROFILES\t1\n", 16)) version = 1;
    else if (!strncmp(buffer, "COOP-PROFILES\t0\n", 16)) version = 0;
    else return 0;
    line = buffer + 16;
    while (*line) {
        char *f[5], *p;
        int n = 1, j;
        uint64_t port, used = 0;
        struct lp_profile *entry;
        next = strchr(line, '\n');
        if (!next || temp.n >= LP_MAX) return 0;
        *next++ = 0;
        f[0] = line;
        for (p = line; *p; p++) if (*p == '\t') { if (n >= 5) return 0; *p = 0; f[n++] = p+1; }
        if (n != (version ? 5 : 3) || !lp_number(f[2], &port) || port > 65535 ||
            (version && !lp_number(f[4], &used))) return 0;
        entry = &temp.p[temp.n];
        if (strlen(f[0]) >= sizeof(entry->name) || strlen(f[1]) >= sizeof(entry->host) ||
            (version && strlen(f[3]) >= sizeof(entry->notes))) return 0;
        strcpy(entry->name, f[0]); strcpy(entry->host, f[1]);
        if (version) strcpy(entry->notes, f[3]);
        entry->port = (unsigned)port; entry->used = used;
        if (!lp_valid(entry)) return 0;
        for (j = 0; j < temp.n; j++) if (!strcmp(temp.p[j].name, entry->name)) return 0;
        temp.n++; line = next;
    }
    *out = temp; return 1;
}
static inline int lp_encode(const struct lp_store *s, char *out, size_t capacity)
{
    size_t used = 0;
    int i, n;
    if (s->n < 0 || s->n > LP_MAX || capacity < 17) return 0;
    strcpy(out, "COOP-PROFILES\t1\n"); used = 16;
    for (i = 0; i < s->n; i++) {
        int j;
        if (!lp_valid(&s->p[i])) return 0;
        for (j = 0; j < i; j++) if (!strcmp(s->p[j].name, s->p[i].name)) return 0;
        n = snprintf(out + used, capacity - used, "%s\t%s\t%u\t%s\t%llu\n", s->p[i].name, s->p[i].host,
                     s->p[i].port, s->p[i].notes, (unsigned long long)s->p[i].used);
        if (n < 0 || (size_t)n >= capacity - used) return 0;
        used += n;
    }
    return (int)used;
}
static inline int lp_due(struct lp_probe *p, uint32_t now)
{
    if (p->pending && (uint32_t)(now - p->sent) >= 1200) {
        p->pending = 0; p->status = LP_OFFLINE; p->version[0] = 0;
    }
    return !p->pending && (int32_t)(now - p->next) >= 0;
}
static inline void lp_sent(struct lp_probe *p, uint32_t now, uint32_t nonce)
{
    p->nonce = nonce; p->sent = now; p->next = now + 15000; p->pending = 1;
}
/* Fixed-width binary probe v1. No names, addresses or membership mutation.
 * Request: CPQ1 + nonce(4). Reply: CPS1 + nonce + count + limit + in-game +
 * version length + version ASCII (1..47). Source endpoint is checked by adapter. */
static inline int lp_reply(struct lp_probe *p, uint32_t now, const unsigned char *b, size_t n, int endpoint, const char *version)
{
    uint32_t nonce;
    size_t i;
    if (n < 13 || n > 59 || memcmp(b, "CPS1", 4)) return 0;
    nonce = (uint32_t)b[4] | (uint32_t)b[5]<<8 | (uint32_t)b[6]<<16 | (uint32_t)b[7]<<24;
    if (!endpoint || !p->pending || nonce != p->nonce || (uint32_t)(now - p->sent) >= 1200) return 0;
    if (!b[9] || b[9] > 16 || b[8] > b[9] || b[10] > 2 || !b[11] || b[11] > 47 || n != 12u + b[11]) return 0;
    for (i = 12; i < n; i++) if (b[i] < 33 || b[i] > 126) return 0;
    memcpy(p->version, b+12, b[11]); p->version[b[11]] = 0;
    p->rtt = (int)(now - p->sent); p->pending = 0;
    p->status = strcmp(p->version, version) ? LP_MISMATCH : b[8] == b[9] ? LP_FULL : b[10] == 2 ? LP_REACHABLE : b[10] ? LP_GAME : LP_LOBBY;
    return 1;
}
static inline int lp_answer(const unsigned char *query, size_t size, unsigned count, int in_game, const char *version, unsigned char *reply)
{
    size_t n = strlen(version);
    if (size != 8 || memcmp(query, "CPQ1", 4) || count > 16 || in_game < 0 || in_game > 2 || !n || n > 47) return 0;
    memcpy(reply, "CPS1", 4); memcpy(reply+4, query+4, 4);
    reply[8] = (unsigned char)count; reply[9] = 16; reply[10] = (unsigned char)in_game; reply[11] = (unsigned char)n;
    memcpy(reply+12, version, n); return (int)(12+n);
}
#endif
