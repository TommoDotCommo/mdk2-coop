/* Hosting core for the launcher lobby: everything game-free and platform-free, so T0 tests run it
 * (bridge/tests/launcher_host_test.c). Portable C99, no allocation, every buffer bounded.
 *
 *   router     NAT-PMP (RFC 6886) requests and replies; UPnP-IGD SSDP search, description and
 *              SOAP messages; what the router's WAN address means (CGNAT, C2)
 *   firewall   which inbound UDP rules let the game in on the Private and Public profiles
 *   invite     the routes an invite carries, best first (C3)
 *   lobby      the hello's lobby key, who may join (invite only, kicked), refusal words, "Waiting for",
 *              which probes an invite-only lobby answers (B9)
 *   pad        recorded XInput states into launcher actions (K3)
 * MIT licence, like the rest of the repository. */
#ifndef LAUNCHER_HOST_CORE_H
#define LAUNCHER_HOST_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "coop_invite.h"

/* ---- router ---- */
#define LH_NATPMP_PORT 5351
#define LH_SSDP_PORT 1900
#define LH_LEASE_S 3600u              /* asked lease; renewed at half */

static inline void lh_be16(uint8_t *b, unsigned v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static inline void lh_be32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }
static inline uint32_t lh_ip4(const uint8_t b[4]) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static inline unsigned lh_rd16(const uint8_t *b) { return (unsigned)b[0] << 8 | b[1]; }
static inline uint32_t lh_rd32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

/* NAT-PMP: external address request (2 bytes) and UDP map request (12 bytes; lifetime 0 deletes). */
static inline int lh_natpmp_addr_req(uint8_t *out) { out[0] = 0; out[1] = 0; return 2; }
static inline int lh_natpmp_map_req(uint8_t *out, unsigned internal, unsigned external, uint32_t lifetime)
{
    out[0] = 0; out[1] = 1; out[2] = out[3] = 0;
    lh_be16(out + 4, internal); lh_be16(out + 6, external); lh_be32(out + 8, lifetime);
    return 12;
}
/* Replies: 0 on a malformed or unexpected packet, else 1 with *result (0 = success). */
static inline int lh_natpmp_addr_reply(const uint8_t *b, size_t n, unsigned *result, uint32_t *ext_ip)
{
    if (n != 12 || b[0] != 0 || b[1] != 128) return 0;
    *result = lh_rd16(b + 2);
    *ext_ip = lh_rd32(b + 8);
    return 1;
}
static inline int lh_natpmp_map_reply(const uint8_t *b, size_t n, unsigned internal, unsigned *result, unsigned *mapped, uint32_t *lifetime)
{
    if (n != 16 || b[0] != 0 || b[1] != 129 || lh_rd16(b + 8) != internal) return 0;
    *result = lh_rd16(b + 2);
    *mapped = lh_rd16(b + 10);
    *lifetime = lh_rd32(b + 12);
    return 1;
}

/* What the router's WAN address means. */
enum lh_wan { LH_WAN_UNKNOWN, LH_WAN_PUBLIC, LH_WAN_PRIVATE, LH_WAN_CGNAT };
static inline int lh_private_ip(uint32_t ip)
{
    return (ip >> 24) == 10 || (ip & 0xfff00000u) == 0xac100000u || (ip & 0xffff0000u) == 0xc0a80000u;
}
static inline int lh_wan_class(uint32_t ip)
{
    if (!ip || ip == 0xffffffffu || (ip >> 24) == 127 || (ip >> 24) == 0 || (ip >> 28) >= 14) return LH_WAN_UNKNOWN;
    if ((ip & 0xffc00000u) == 0x64400000u) return LH_WAN_CGNAT;            /* 100.64.0.0/10 */
    if (lh_private_ip(ip) || (ip & 0xffff0000u) == 0xa9fe0000u) return LH_WAN_PRIVATE;
    return LH_WAN_PUBLIC;
}

/* Case-insensitive search in a bounded buffer. */
static inline const char *lh_find(const char *s, size_t n, const char *word)
{
    size_t k, w = strlen(word), i;
    if (!w || w > n) return NULL;
    for (i = 0; i + w <= n; i++) {
        for (k = 0; k < w && ci_upper((unsigned char)s[i + k]) == ci_upper((unsigned char)word[k]); k++) {}
        if (k == w) return s + i;
    }
    return NULL;
}
static inline int lh_ssdp_search(char *out, size_t cap, const char *host)
{
    int n = snprintf(out, cap, "M-SEARCH * HTTP/1.1\r\nHOST: %s:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                     "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n", host);
    return n > 0 && (size_t)n < cap ? n : 0;
}
/* http://a.b.c.d[:port]/path (IPv4 only, the form routers send). */
struct lh_url { uint32_t ip; unsigned port; char path[256]; };
static inline int lh_parse_url(const char *s, size_t n, struct lh_url *u)
{
    uint8_t ip[4];
    size_t used = 0, i;
    unsigned port = 80;
    memset(u, 0, sizeof(*u));
    if (n < 8 || lh_find(s, 7, "http://") != s) return 0;
    s += 7; n -= 7;
    if (!cr_ipv4(s, n, ip, &used)) return 0;
    i = used;
    if (i < n && s[i] == ':') {
        port = 0;
        for (i++; i < n && s[i] >= '0' && s[i] <= '9' && port < 65536; i++) port = port * 10 + (unsigned)(s[i] - '0');
        if (!port || port > 65535) return 0;
    }
    if (i < n && s[i] != '/') return 0;
    if (n - i >= sizeof(u->path)) return 0;
    if (i == n) strcpy(u->path, "/");
    else { memcpy(u->path, s + i, n - i); u->path[n - i] = 0; }
    for (i = 0; u->path[i]; i++) if ((unsigned char)u->path[i] < 33 || u->path[i] == 127) return 0;
    u->ip = lh_ip4(ip);
    u->port = port;
    return 1;
}
/* The LOCATION header of an SSDP answer. */
static inline int lh_ssdp_location(const char *s, size_t n, struct lh_url *u)
{
    const char *p = lh_find(s, n, "\nLOCATION:"), *e;
    if (!p || (n < 12 || (memcmp(s, "HTTP/1.1 200", 12) && memcmp(s, "HTTP/1.0 200", 12)))) return 0;
    p += 10;
    while (p < s + n && (*p == ' ' || *p == '\t')) p++;
    for (e = p; e < s + n && *e != '\r' && *e != '\n'; e++) {}
    while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
    return lh_parse_url(p, (size_t)(e - p), u);
}
/* The text of the first <tag>...</tag> in s[0..n) (bounded, trimmed); 0 if absent or too long. */
static inline int lh_xml_text(const char *s, size_t n, const char *tag, char *out, size_t cap)
{
    char open[48], close[48];
    const char *a, *b;
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    if (!(a = lh_find(s, n, open))) return 0;
    a += strlen(open);
    if (!(b = lh_find(a, (size_t)(s + n - a), close)) || (size_t)(b - a) >= cap) return 0;
    while (a < b && ci_space((unsigned char)*a)) a++;
    while (b > a && ci_space((unsigned char)b[-1])) b--;
    memcpy(out, a, (size_t)(b - a));
    out[b - a] = 0;
    return 1;
}
/* From the IGD description: the WAN connection service and its control URL (absolute, resolved
 * against URLBase or the description's own address). WANIPConnection 2, 1, then WANPPPConnection 1. */
struct lh_igd { struct lh_url control; char service[80]; };
static inline int lh_igd_parse(const char *xml, size_t n, const struct lh_url *where, struct lh_igd *g)
{
    static const char *services[] = { "urn:schemas-upnp-org:service:WANIPConnection:2",
                                      "urn:schemas-upnp-org:service:WANIPConnection:1",
                                      "urn:schemas-upnp-org:service:WANPPPConnection:1" };
    char control[256], base[256];
    int k;
    memset(g, 0, sizeof(*g));
    for (k = 0; k < 3; k++) {
        const char *at = xml, *st, *end;
        while ((st = lh_find(at, (size_t)(xml + n - at), "<serviceType>")) != NULL) {
            char type[96];
            const char *svc_end = lh_find(st, (size_t)(xml + n - st), "</service>");
            end = svc_end ? svc_end : xml + n;
            at = st + 13;
            if (!lh_xml_text(st, (size_t)(end - st), "serviceType", type, sizeof(type)) || strcmp(type, services[k])) continue;
            if (!lh_xml_text(st, (size_t)(end - st), "controlURL", control, sizeof(control) - 1)) continue;
            snprintf(g->service, sizeof(g->service), "%s", services[k]);
            if (lh_parse_url(control, strlen(control), &g->control)) return 1;
            if (control[0] != '/') { memmove(control + 1, control, strlen(control) + 1); control[0] = '/'; }
            g->control = *where;
            if (lh_xml_text(xml, n, "URLBase", base, sizeof(base))) {
                struct lh_url b;
                if (lh_parse_url(base, strlen(base), &b)) g->control = b;
            }
            if (strlen(control) >= sizeof(g->control.path)) return 0;
            strcpy(g->control.path, control);
            return 1;
        }
    }
    return 0;
}
/* SOAP request for AddPortMapping / DeletePortMapping / GetExternalIPAddress: the whole HTTP text. */
enum { LH_SOAP_ADD, LH_SOAP_DELETE, LH_SOAP_EXTIP };
static inline int lh_soap(char *out, size_t cap, const struct lh_igd *g, int what, unsigned port, const char *client, unsigned lease)
{
    static const char *names[] = { "AddPortMapping", "DeletePortMapping", "GetExternalIPAddress" };
    char args[640], body[1024];
    int n;
    if (what == LH_SOAP_ADD)
        snprintf(args, sizeof(args), "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                 "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
                 "<NewPortMappingDescription>MDK2 Co-op</NewPortMappingDescription><NewLeaseDuration>%u</NewLeaseDuration>",
                 port, port, client, lease);
    else if (what == LH_SOAP_DELETE)
        snprintf(args, sizeof(args), "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>", port);
    else
        args[0] = 0;
    n = snprintf(body, sizeof(body), "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                 "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
                 names[what], g->service, args, names[what]);
    if (n <= 0 || (size_t)n >= sizeof(body)) return 0;
    n = snprintf(out, cap, "POST %s HTTP/1.1\r\nHOST: %u.%u.%u.%u:%u\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                 "SOAPACTION: \"%s#%s\"\r\nCONTENT-LENGTH: %d\r\nCONNECTION: close\r\n\r\n%s",
                 g->control.path, g->control.ip >> 24, g->control.ip >> 16 & 255, g->control.ip >> 8 & 255, g->control.ip & 255,
                 g->control.port, g->service, names[what], n, body);
    return n > 0 && (size_t)n < cap ? n : 0;
}
/* An HTTP answer: its status (0 if not HTTP), the UPnP errorCode of a fault, the external address. */
struct lh_soap_answer { int status, error; uint32_t ext_ip; char description[64]; };
static inline int lh_soap_parse(const char *s, size_t n, struct lh_soap_answer *a)
{
    char text[64];
    uint8_t ip[4];
    size_t used;
    memset(a, 0, sizeof(*a));
    if (n < 12 || memcmp(s, "HTTP/1.", 7) || s[8] != ' ' || s[9] < '1' || s[9] > '5' ||
        s[10] < '0' || s[10] > '9' || s[11] < '0' || s[11] > '9') return 0;
    a->status = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    if (lh_xml_text(s, n, "errorCode", text, sizeof(text))) a->error = atoi(text);
    lh_xml_text(s, n, "errorDescription", a->description, sizeof(a->description));
    if (lh_xml_text(s, n, "NewExternalIPAddress", text, sizeof(text)) && cr_ipv4(text, strlen(text), ip, &used) && used == strlen(text))
        a->ext_ip = lh_ip4(ip);
    return 1;
}

/* ---- firewall: inbound UDP for the game on the Private and Public profiles ---- */
#define LH_FW_DOMAIN 1
#define LH_FW_PRIVATE 2
#define LH_FW_PUBLIC 4
struct lh_fw_rule {
    int enabled, inbound, allow, protocol;   /* protocol 17 UDP, 6 TCP, 256 any */
    long profiles;                            /* LH_FW_* bits (0x7fffffff: all) */
    char app[260];                            /* empty: every program */
    char ports[128];                          /* "", "*" or "Any": every port; else "27900,27901-27903" */
};
enum lh_fw_verdict { LH_FW_NO_RULE, LH_FW_ALLOWED, LH_FW_BLOCKED, LH_FW_OFF };
static inline int lh_path_same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = ci_upper((unsigned char)*a), y = ci_upper((unsigned char)*b);
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y) return 0;
    }
    return !*a && !*b;
}
static inline int lh_port_in(const char *list, unsigned port)
{
    const char *p = list;
    if (!list[0] || !strcmp(list, "*") || lh_path_same(list, "Any")) return 1;
    while (*p) {
        unsigned lo = 0, hi;
        int digits = 0;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9' && lo < 70000) { lo = lo * 10 + (unsigned)(*p++ - '0'); digits++; }
        hi = lo;
        if (*p == '-') {
            p++; hi = 0;
            while (*p >= '0' && *p <= '9' && hi < 70000) hi = hi * 10 + (unsigned)(*p++ - '0');
        }
        if (digits && port >= lo && port <= hi) return 1;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return 0;
}
/* Verdict for one profile: a matching block rule wins over an allow rule (as Windows does). */
static inline int lh_fw_eval(const struct lh_fw_rule *r, int n, const char *exe, unsigned port, int profile, int firewall_on)
{
    int i, allowed = 0;
    if (!firewall_on) return LH_FW_OFF;
    for (i = 0; i < n; i++) {
        if (!r[i].enabled || !r[i].inbound || !(r[i].profiles & profile)) continue;
        if (r[i].protocol != 17 && r[i].protocol != 256) continue;
        if (r[i].app[0] && !lh_path_same(r[i].app, exe)) continue;
        if (!lh_port_in(r[i].ports, port)) continue;
        if (!r[i].allow) return LH_FW_BLOCKED;
        allowed = 1;
    }
    return allowed ? LH_FW_ALLOWED : LH_FW_NO_RULE;
}
/* The panel's line for the two profiles, and whether it is a pass. */
static inline int lh_fw_line(int private_verdict, int public_verdict, char *out, size_t cap)
{
    static const char *words[] = { "no rule (Windows will ask)", "allowed", "blocked", "firewall off" };
    int ok = (private_verdict == LH_FW_ALLOWED || private_verdict == LH_FW_OFF) &&
             (public_verdict == LH_FW_ALLOWED || public_verdict == LH_FW_OFF);
    snprintf(out, cap, "Private networks: %s. Public networks: %s.", words[private_verdict & 3], words[public_verdict & 3]);
    return ok;
}

/* ---- invite routes, best first: up to two LAN addresses, Tailscale, then the internet ---- */
struct lh_addr { uint32_t ip; int prefix; };
static inline void lh_ip_put(uint8_t *b, uint32_t ip) { b[0] = (uint8_t)(ip >> 24); b[1] = (uint8_t)(ip >> 16); b[2] = (uint8_t)(ip >> 8); b[3] = (uint8_t)ip; }
static inline int lh_invite_routes(struct ci_invite *v, const struct lh_addr *a, int n, uint32_t tailscale_ip, uint32_t internet_ip, int internet_ok)
{
    int i, lan = 0, pass;
    v->n = 0;
    for (pass = 0; pass < 2; pass++)            /* RFC 1918 first, then link-local (one cable) */
        for (i = 0; i < n && lan < 2; i++) {
            int fit = pass ? (a[i].ip & 0xffff0000u) == 0xa9fe0000u : lh_private_ip(a[i].ip);
            if (!fit) continue;
            v->route[v->n].kind = CI_LAN; lh_ip_put(v->route[v->n].ip, a[i].ip); v->n++; lan++;
        }
    if (tailscale_ip && (tailscale_ip & 0xffc00000u) == 0x64400000u) {
        v->route[v->n].kind = CI_VPN; lh_ip_put(v->route[v->n].ip, tailscale_ip); v->n++;
    }
    if (internet_ok && lh_wan_class(internet_ip) == LH_WAN_PUBLIC && v->n < CI_MAX_ROUTES) {
        v->route[v->n].kind = CI_INTERNET; lh_ip_put(v->route[v->n].ip, internet_ip); v->n++;
    }
    return v->n;
}

/* ---- lobby: hello, admission, refusal, "Waiting for", probes ---- */
/* "MDKL H <id> <ready> <rtt> <ts> <version>[ <key hex>]|<name>"; the key is the invite's lobby secret. */
struct lh_hello { unsigned id; int ready, rtt, has_key; unsigned long ts; uint32_t key; char version[48], name[33]; };
static inline int lh_hello_parse(const char *s, struct lh_hello *h)
{
    const char *bar = strchr(s, '|');
    unsigned key = 0;
    int got;
    memset(h, 0, sizeof(*h));
    if (strncmp(s, "MDKL H ", 7) || !bar) return 0;
    got = sscanf(s + 7, "%x %d %d %lu %47[^| ] %x", &h->id, &h->ready, &h->rtt, &h->ts, h->version, &key);
    if (got < 5) return 0;
    h->has_key = got == 6;
    h->key = key;
    snprintf(h->name, sizeof(h->name), "%s", bar + 1);
    return 1;
}
static inline int lh_hello_write(char *out, size_t cap, unsigned id, int ready, int rtt, unsigned long ts, const char *version,
                                 int has_key, uint32_t key, const char *name)
{
    int n = has_key ? snprintf(out, cap, "MDKL H %x %d %d %lu %s %x|%s", id, ready, rtt, ts, version, (unsigned)key, name)
                    : snprintf(out, cap, "MDKL H %x %d %d %lu %s|%s", id, ready, rtt, ts, version, name);
    return n > 0 && (size_t)n < cap ? n : 0;
}
#define LH_BANS 32
/* Kicked this lobby: by launcher id, by address and port, and by name (a rejoin from the same launcher
 * gets a new id and port; the name stays). Another player on the same PC is not affected. */
struct lh_bans { int n; struct { unsigned id; uint32_t ip; unsigned port; char name[33]; } b[LH_BANS]; };
static inline void lh_ban(struct lh_bans *b, unsigned id, uint32_t ip, unsigned port, const char *name)
{
    int k = b->n < LH_BANS ? b->n++ : LH_BANS - 1;
    b->b[k].id = id; b->b[k].ip = ip; b->b[k].port = port;
    snprintf(b->b[k].name, sizeof(b->b[k].name), "%s", name ? name : "");
}
static inline int lh_banned(const struct lh_bans *b, unsigned id, uint32_t ip, unsigned port, const char *name)
{
    int i;
    for (i = 0; i < b->n; i++) {
        if (b->b[i].id == id || (b->b[i].ip == ip && b->b[i].port == port)) return 1;
        if (name && name[0] && b->b[i].name[0] && lh_path_same(b->b[i].name, name)) return 1;
    }
    return 0;
}
enum lh_refuse { LH_ADMIT, LH_REFUSE_KICKED, LH_REFUSE_INVITE, LH_REFUSE_FULL };
/* Who may come in: not kicked this session, the lobby's key when it is invite only, room left. */
static inline int lh_admit(const struct lh_bans *bans, int invite_only, uint32_t secret, const struct lh_hello *h,
                           uint32_t ip, unsigned port, int members, int limit)
{
    if (lh_banned(bans, h->id, ip, port, h->name)) return LH_REFUSE_KICKED;
    if (invite_only && (!h->has_key || h->key != secret)) return LH_REFUSE_INVITE;
    if (members >= limit) return LH_REFUSE_FULL;
    return LH_ADMIT;
}
static inline const char *lh_refusal_text(int reason)
{
    return reason == LH_REFUSE_KICKED ? "The host removed you from this lobby." :
           reason == LH_REFUSE_INVITE ? "This lobby is invite only. Ask the host for the whole invite." :
           reason == LH_REFUSE_FULL ? "The lobby is full." : "";
}
/* "MDKL K <reason>": the host refuses or removes a player. */
static inline int lh_refusal_parse(const char *s, size_t n)
{
    if (n < 8 || strncmp(s, "MDKL K ", 7) || s[7] < '1' || s[7] > '3') return 0;
    return s[7] - '0';
}
/* B9: an invite-only lobby is not listed: it does not answer the v1 probe (saved addresses list from it),
 * and answers the v2 probe with the invite-only flag and no name (the joiner shows it only for its invite). */
enum { LH_PROBE_V1, LH_PROBE_V2 };
static inline int lh_probe_allowed(int invite_only, int version)
{
    return version == LH_PROBE_V2 || !invite_only;
}
/* "Waiting for Fox", "Waiting for Fox and Bob", "Waiting for Fox, Bob and 2 more"; "" when all are ready. */
static inline int lh_waiting(char *out, size_t cap, const char *const *names, const int *ready, int n)
{
    int i, waiting = 0, shown = 0;
    size_t used;
    out[0] = 0;
    for (i = 0; i < n; i++) waiting += !ready[i];
    if (!waiting || cap < 16) return 0;
    used = (size_t)snprintf(out, cap, "Waiting for ");
    for (i = 0; i < n && used < cap; i++) {
        const char *sep;
        if (ready[i]) continue;
        if (shown == 2 && waiting > 3) { snprintf(out + used, cap - used, " and %d more", waiting - 2); return waiting; }
        sep = !shown ? "" : shown == waiting - 1 ? " and " : ", ";
        used += (size_t)snprintf(out + used, cap - used, "%s%.24s", sep, names[i]);
        shown++;
    }
    return waiting;
}

/* ---- pad (K3): recorded XInput states into launcher actions ---- */
#define LH_PAD_DPAD_UP 0x0001
#define LH_PAD_DPAD_DOWN 0x0002
#define LH_PAD_DPAD_LEFT 0x0004
#define LH_PAD_DPAD_RIGHT 0x0008
#define LH_PAD_START 0x0010
#define LH_PAD_BACK 0x0020
#define LH_PAD_LB 0x0100
#define LH_PAD_RB 0x0200
#define LH_PAD_A 0x1000
#define LH_PAD_B 0x2000
#define LH_PAD_X 0x4000
#define LH_PAD_Y 0x8000
#define LH_PAD_DEADZONE 16000
#define LH_PAD_DELAY 400u              /* a held direction repeats after this */
#define LH_PAD_REPEAT 120u
enum lh_pad_action { LH_ACT_NONE, LH_ACT_UP, LH_ACT_DOWN, LH_ACT_LEFT, LH_ACT_RIGHT, LH_ACT_CONFIRM, LH_ACT_BACK,
                     LH_ACT_TAB_PREV, LH_ACT_TAB_NEXT, LH_ACT_PRIMARY, LH_ACT_X, LH_ACT_Y_DOWN, LH_ACT_Y_UP };
struct lh_pad { unsigned held; uint32_t next[4]; };
/* The stick counts as the D-pad past the dead zone. */
static inline unsigned lh_pad_buttons(unsigned buttons, int lx, int ly)
{
    if (ly > LH_PAD_DEADZONE) buttons |= LH_PAD_DPAD_UP;
    if (ly < -LH_PAD_DEADZONE) buttons |= LH_PAD_DPAD_DOWN;
    if (lx < -LH_PAD_DEADZONE) buttons |= LH_PAD_DPAD_LEFT;
    if (lx > LH_PAD_DEADZONE) buttons |= LH_PAD_DPAD_RIGHT;
    return buttons & 0xffff;
}
/* One sample: writes up to `cap` actions (presses, Y release, direction repeats); returns the count. */
static inline int lh_pad_step(struct lh_pad *p, unsigned buttons, int lx, int ly, uint32_t now, int *out, int cap)
{
    static const unsigned dirs[4] = { LH_PAD_DPAD_UP, LH_PAD_DPAD_DOWN, LH_PAD_DPAD_LEFT, LH_PAD_DPAD_RIGHT };
    static const struct { unsigned bit; int act; } presses[] = {
        { LH_PAD_A, LH_ACT_CONFIRM }, { LH_PAD_B, LH_ACT_BACK }, { LH_PAD_LB, LH_ACT_TAB_PREV }, { LH_PAD_RB, LH_ACT_TAB_NEXT },
        { LH_PAD_START, LH_ACT_PRIMARY }, { LH_PAD_X, LH_ACT_X }, { LH_PAD_Y, LH_ACT_Y_DOWN } };
    unsigned cur = lh_pad_buttons(buttons, lx, ly), down = cur & ~p->held;
    int n = 0, k;
    for (k = 0; k < 4; k++) {
        if (down & dirs[k]) {
            if (n < cap) out[n++] = LH_ACT_UP + k;
            p->next[k] = now + LH_PAD_DELAY;
        } else if ((cur & dirs[k]) && (int32_t)(now - p->next[k]) >= 0) {
            if (n < cap) out[n++] = LH_ACT_UP + k;
            p->next[k] = now + LH_PAD_REPEAT;
        }
    }
    for (k = 0; k < (int)(sizeof(presses) / sizeof(presses[0])); k++)
        if ((down & presses[k].bit) && n < cap) out[n++] = presses[k].act;
    if ((p->held & LH_PAD_Y) && !(cur & LH_PAD_Y) && n < cap) out[n++] = LH_ACT_Y_UP;
    p->held = cur;
    return n;
}
/* LB/RB: the next tab, wrapping (four tabs: Play, Saves, Settings, About). */
static inline int lh_tab_step(int tab, int tabs, int action)
{
    if (action == LH_ACT_TAB_NEXT) return (tab + 1) % tabs;
    if (action == LH_ACT_TAB_PREV) return (tab + tabs - 1) % tabs;
    return tab;
}
#endif
