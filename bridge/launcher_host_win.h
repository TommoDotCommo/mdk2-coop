/* Windows side of the launcher's hosting checks (launcher_host_core.h has the wire formats):
 *   router    map the lobby's UDP port with UPnP-IGD, else NAT-PMP; renew at half the lease; remove it
 *   firewall  read (never change) the Windows Firewall rules for the game through INetFwPolicy2
 * Plain Winsock; iphlpapi, ole32 and oleaut32 are loaded at run time from the system folder (no new imports).
 * Everything blocks for at most the timeouts given: call it from a worker thread.
 * bridge/tests/launcher_host_win_test.c runs it against a fake IGD and NAT-PMP responder. MIT licence. */
#ifndef LAUNCHER_HOST_WIN_H
#define LAUNCHER_HOST_WIN_H
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#define COBJMACROS
#include <netfw.h>
#include "launcher_host_core.h"

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

static inline HMODULE lhw_system_dll(const char *name)
{
    HMODULE m = LoadLibraryExA(name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? m : LoadLibraryA(name);
}

/* ---- sockets ---- */
static inline void lhw_sin(struct sockaddr_in *a, uint32_t ip, unsigned port)
{
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = htonl(ip);
    a->sin_port = htons((u_short)port);
}
static inline DWORD lhw_left(DWORD start, DWORD ms)
{
    DWORD gone = GetTickCount() - start;
    return gone < ms ? ms - gone : 0;
}
static inline int lhw_wait(SOCKET s, int for_write, DWORD ms)
{
    fd_set set;
    struct timeval tv;
    FD_ZERO(&set);
    FD_SET(s, &set);
    tv.tv_sec = (long)(ms / 1000);
    tv.tv_usec = (long)(ms % 1000) * 1000;
    return select(0, for_write ? NULL : &set, for_write ? &set : NULL, NULL, &tv) > 0;
}
/* One UDP question, the first answer from that address within ms (tries: resends, the wait doubling). */
static inline int lhw_udp_ask(uint32_t ip, unsigned port, const void *q, int qn, void *out, int cap, DWORD ms, int tries)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in to, from;
    int n = -1, k;
    if (s == INVALID_SOCKET) return -1;
    lhw_sin(&to, ip, port);
    for (k = 0; k < tries && n < 0; k++) {
        DWORD start = GetTickCount(), wait = ms << k;
        sendto(s, (const char *)q, qn, 0, (struct sockaddr *)&to, sizeof(to));
        while (lhw_left(start, wait) && lhw_wait(s, 0, lhw_left(start, wait))) {
            int fl = sizeof(from), got = recvfrom(s, (char *)out, cap, 0, (struct sockaddr *)&from, &fl);
            if (got > 0 && from.sin_addr.s_addr == to.sin_addr.s_addr && from.sin_port == to.sin_port) { n = got; break; }
        }
    }
    closesocket(s);
    return n;
}
/* One HTTP exchange over TCP (Connection: close): the whole answer in out, NUL-terminated. */
static inline int lhw_http(uint32_t ip, unsigned port, const char *req, int qn, char *out, int cap, DWORD ms)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in to;
    u_long nb = 1;
    int got = 0, sent = 0;
    DWORD start = GetTickCount();
    if (s == INVALID_SOCKET) return -1;
    lhw_sin(&to, ip, port);
    ioctlsocket(s, FIONBIO, &nb);
    if (connect(s, (struct sockaddr *)&to, sizeof(to)) != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return -1; }
    if (!lhw_wait(s, 1, ms)) { closesocket(s); return -1; }
    while (sent < qn && lhw_left(start, ms)) {
        int k = send(s, req + sent, qn - sent, 0);
        if (k > 0) sent += k;
        else if (WSAGetLastError() == WSAEWOULDBLOCK) lhw_wait(s, 1, 50);
        else break;
    }
    if (sent < qn) { closesocket(s); return -1; }
    while (got < cap - 1 && lhw_left(start, ms)) {
        int k;
        if (!lhw_wait(s, 0, lhw_left(start, ms))) break;
        k = recv(s, out + got, cap - 1 - got, 0);
        if (k <= 0) break;
        got += k;
    }
    closesocket(s);
    out[got] = 0;
    return got;
}
/* This PC's address towards ip (the local end of a "connected" UDP socket: nothing is sent). */
static inline uint32_t lhw_local_ip_to(uint32_t ip)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in to, me;
    int len = sizeof(me);
    uint32_t r = 0;
    if (s == INVALID_SOCKET) return 0;
    lhw_sin(&to, ip, 9);
    if (connect(s, (struct sockaddr *)&to, sizeof(to)) == 0 && getsockname(s, (struct sockaddr *)&me, &len) == 0)
        r = ntohl(me.sin_addr.s_addr);
    closesocket(s);
    return r;
}
/* The default gateway (the next hop towards the internet), or 0. */
static inline uint32_t lhw_gateway(void)
{
    typedef DWORD (WINAPI *best_fn)(DWORD, DWORD, PMIB_IPFORWARDROW);
    HMODULE lib = lhw_system_dll("iphlpapi.dll");
    best_fn best = lib ? (best_fn)(void (*)(void))GetProcAddress(lib, "GetBestRoute") : NULL;
    MIB_IPFORWARDROW row;
    uint32_t gw = 0;
    if (best && best(inet_addr("8.8.8.8"), 0, &row) == NO_ERROR) gw = ntohl(row.dwForwardNextHop);
    if (lib) FreeLibrary(lib);
    return gw;
}

/* ---- router ---- */
struct lhw_router_cfg {
    uint32_t ssdp_ip;       /* 0: 239.255.255.250 (tests: a loopback fake) */
    unsigned ssdp_port;     /* 0: 1900 */
    uint32_t natpmp_ip;     /* 0: the default gateway */
    unsigned natpmp_port;   /* 0: 5351 */
    unsigned lease;         /* 0: LH_LEASE_S */
    DWORD timeout_ms;       /* each question; 0: 1500 */
};
enum { LHW_NONE, LHW_UPNP, LHW_NATPMP };
struct lhw_router {
    int method, upnp_answered, natpmp_answered, wan, upnp_error;
    unsigned port, natpmp_result, lease;
    uint32_t ext_ip, client_ip;
    DWORD mapped_at;
    struct lh_igd igd;
    char said[200];         /* what the router said, for the panel and the log */
};
static inline DWORD lhw_timeout(const struct lhw_router_cfg *c) { return c->timeout_ms ? c->timeout_ms : 1500; }
static inline int lhw_upnp_find(struct lhw_router *r, const struct lhw_router_cfg *c)
{
    static char buf[16384];
    char req[512];
    struct sockaddr_in to, from;
    struct lh_url where;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int n, ttl = 2, found = 0, k;
    DWORD start, ms = lhw_timeout(c) / 2 + 1;
    if (s == INVALID_SOCKET) return 0;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
    lhw_sin(&to, c->ssdp_ip ? c->ssdp_ip : 0xeffffffau, c->ssdp_port ? c->ssdp_port : LH_SSDP_PORT);
    n = lh_ssdp_search(req, sizeof(req), "239.255.255.250");
    for (k = 0; k < 2 && !found; k++) {
        sendto(s, req, n, 0, (struct sockaddr *)&to, sizeof(to));
        start = GetTickCount();
        while (!found && lhw_left(start, ms) && lhw_wait(s, 0, lhw_left(start, ms))) {
            int fl = sizeof(from), got = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
            if (got > 0 && lh_ssdp_location(buf, (size_t)got, &where)) found = 1;
        }
    }
    closesocket(s);
    if (!found) return 0;
    r->upnp_answered = 1;
    n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHOST: %u.%u.%u.%u:%u\r\nCONNECTION: close\r\n\r\n", where.path,
                 where.ip >> 24, where.ip >> 16 & 255, where.ip >> 8 & 255, where.ip & 255, where.port);
    if (n <= 0 || n >= (int)sizeof(req)) return 0;
    n = lhw_http(where.ip, where.port, req, n, buf, sizeof(buf), lhw_timeout(c) * 2);
    if (n <= 0 || !lh_igd_parse(buf, (size_t)n, &where, &r->igd)) {
        snprintf(r->said, sizeof(r->said), "The router answered UPnP but has no internet connection service.");
        return 0;
    }
    return 1;
}
static inline int lhw_upnp_call(struct lhw_router *r, const struct lhw_router_cfg *c, int what, unsigned lease, struct lh_soap_answer *a)
{
    char req[2048], buf[8192], client[16];
    int n;
    memset(a, 0, sizeof(*a));
    snprintf(client, sizeof(client), "%u.%u.%u.%u", r->client_ip >> 24, r->client_ip >> 16 & 255, r->client_ip >> 8 & 255, r->client_ip & 255);
    if (!(n = lh_soap(req, sizeof(req), &r->igd, what, r->port, client, lease))) return 0;
    n = lhw_http(r->igd.control.ip, r->igd.control.port, req, n, buf, sizeof(buf), lhw_timeout(c) * 2);
    return n > 0 && lh_soap_parse(buf, (size_t)n, a) && a->status == 200;
}
static inline int lhw_upnp_map(struct lhw_router *r, const struct lhw_router_cfg *c)
{
    struct lh_soap_answer a;
    unsigned lease = c->lease ? c->lease : LH_LEASE_S;
    if (!lhw_upnp_call(r, c, LH_SOAP_ADD, lease, &a)) {
        if (a.error == 725 && lhw_upnp_call(r, c, LH_SOAP_ADD, 0, &a)) lease = 0;     /* only permanent leases */
        else {
            r->upnp_error = a.error;
            snprintf(r->said, sizeof(r->said), "The router refused the UPnP port mapping (%d %s).", a.error ? a.error : a.status,
                     a.description[0] ? a.description : "no reason given");
            return 0;
        }
    }
    r->lease = lease;
    r->method = LHW_UPNP;
    if (lhw_upnp_call(r, c, LH_SOAP_EXTIP, 0, &a)) r->ext_ip = a.ext_ip;
    return 1;
}
static inline int lhw_natpmp_map(struct lhw_router *r, const struct lhw_router_cfg *c, uint32_t lifetime)
{
    uint8_t q[12], a[16];
    uint32_t gw = c->natpmp_ip ? c->natpmp_ip : lhw_gateway(), ext = 0, life = 0;
    unsigned port = c->natpmp_port ? c->natpmp_port : LH_NATPMP_PORT, result = 0, mapped = 0;
    int n;
    DWORD ms = lhw_timeout(c) / 4 + 1;
    if (!gw) return 0;
    if (lifetime && !r->ext_ip) {
        n = lhw_udp_ask(gw, port, q, lh_natpmp_addr_req(q), a, sizeof(a), ms, 2);
        if (n > 0 && lh_natpmp_addr_reply(a, (size_t)n, &result, &ext)) { r->natpmp_answered = 1; if (!result) r->ext_ip = ext; }
    }
    n = lhw_udp_ask(gw, port, q, lh_natpmp_map_req(q, r->port, lifetime ? r->port : 0, lifetime), a, sizeof(a), ms, 2);
    if (n <= 0 || !lh_natpmp_map_reply(a, (size_t)n, r->port, &result, &mapped, &life)) return 0;
    r->natpmp_answered = 1;
    r->natpmp_result = result;
    if (result) {
        snprintf(r->said, sizeof(r->said), "The router refused the NAT-PMP mapping (result %u).", result);
        return 0;
    }
    if (lifetime) { r->method = LHW_NATPMP; r->lease = life; }
    return 1;
}
/* Map port (UDP). Returns 1 when mapped; r->said and r->wan say what the router said. */
static inline int lhw_router_open(struct lhw_router *r, const struct lhw_router_cfg *c, unsigned port)
{
    memset(r, 0, sizeof(*r));
    r->port = port;
    if (lhw_upnp_find(r, c)) {
        r->client_ip = lhw_local_ip_to(r->igd.control.ip);
        lhw_upnp_map(r, c);
    }
    if (r->method == LHW_NONE) lhw_natpmp_map(r, c, c->lease ? c->lease : LH_LEASE_S);
    r->wan = lh_wan_class(r->ext_ip);
    if (r->method != LHW_NONE) {
        r->mapped_at = GetTickCount();
        snprintf(r->said, sizeof(r->said), "The router opened UDP %u with %s.", port, r->method == LHW_UPNP ? "UPnP" : "NAT-PMP");
    } else if (!r->said[0]) {
        snprintf(r->said, sizeof(r->said), "%s", r->upnp_answered || r->natpmp_answered ? "The router did not open the port." :
                 "No answer from the router to UPnP or NAT-PMP (it may have them turned off).");
    }
    return r->method != LHW_NONE;
}
/* Renew at half the lease (a lease of 0 is permanent). */
static inline int lhw_router_renew(struct lhw_router *r, const struct lhw_router_cfg *c, DWORD now)
{
    if (r->method == LHW_NONE || !r->lease || now - r->mapped_at < r->lease * 500u) return 0;
    if (r->method == LHW_UPNP ? lhw_upnp_map(r, c) : lhw_natpmp_map(r, c, r->lease)) r->mapped_at = now;
    return 1;
}
/* Remove the mapping (lobby closed, launcher leaving). */
static inline void lhw_router_close(struct lhw_router *r, const struct lhw_router_cfg *c)
{
    struct lh_soap_answer a;
    if (r->method == LHW_UPNP) lhw_upnp_call(r, c, LH_SOAP_DELETE, 0, &a);
    else if (r->method == LHW_NATPMP) lhw_natpmp_map(r, c, 0);
    r->method = LHW_NONE;
}
/* After the lobby handed the port to the game: renewed while the game runs, removed when *end is set
 * (the game quits). */
static inline void lhw_router_keep(struct lhw_router *r, const struct lhw_router_cfg *c, volatile LONG *end, DWORD step_ms)
{
    while (!InterlockedCompareExchange(end, 0, 0)) {
        Sleep(step_ms);
        lhw_router_renew(r, c, GetTickCount());
    }
    lhw_router_close(r, c);
}

/* ---- firewall (read only) ---- */
#define LHW_FW_RULES 64
struct lhw_fw { int ok, private_verdict, public_verdict; long current; int rules_seen, rules_kept; char error[96]; };
static inline void lhw_bstr(BSTR b, char *out, int cap)
{
    char raw[600];
    out[0] = 0;
    if (!b || !WideCharToMultiByte(CP_UTF8, 0, b, -1, raw, sizeof(raw), NULL, NULL)) return;
    if (strchr(raw, '%')) {                       /* "%ProgramFiles%\..." as rules often store it */
        DWORD k = ExpandEnvironmentStringsA(raw, out, (DWORD)cap);
        if (k && k <= (DWORD)cap) return;
    }
    snprintf(out, (size_t)cap, "%s", raw);
}
static inline int lhw_firewall(const char *exe, unsigned port, struct lhw_fw *fw)
{
    static const GUID clsid = { 0xE2B3C97F, 0x6AE1, 0x41AC, { 0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD } };
    static const GUID iid_policy = { 0x98325047, 0xC671, 0x4174, { 0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86 } };
    static const GUID iid_rule = { 0xAF230D27, 0xBABA, 0x4E42, { 0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2 } };
    static const GUID iid_enum = { 0x00020404, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
    typedef HRESULT (WINAPI *init_fn)(void *, DWORD);
    typedef HRESULT (WINAPI *create_fn)(REFCLSID, void *, DWORD, REFIID, void **);
    typedef void (WINAPI *uninit_fn)(void);
    typedef void (WINAPI *free_fn)(BSTR);
    typedef HRESULT (WINAPI *clear_fn)(VARIANT *);
    static struct lh_fw_rule rules[LHW_FW_RULES];
    HMODULE ole = lhw_system_dll("ole32.dll"), aut = lhw_system_dll("oleaut32.dll");
    init_fn co_init = ole ? (init_fn)(void (*)(void))GetProcAddress(ole, "CoInitializeEx") : NULL;
    create_fn co_create = ole ? (create_fn)(void (*)(void))GetProcAddress(ole, "CoCreateInstance") : NULL;
    uninit_fn co_uninit = ole ? (uninit_fn)(void (*)(void))GetProcAddress(ole, "CoUninitialize") : NULL;
    free_fn sys_free = aut ? (free_fn)(void (*)(void))GetProcAddress(aut, "SysFreeString") : NULL;
    clear_fn var_clear = aut ? (clear_fn)(void (*)(void))GetProcAddress(aut, "VariantClear") : NULL;
    INetFwPolicy2 *policy = NULL;
    INetFwRules *list = NULL;
    IUnknown *unk = NULL;
    IEnumVARIANT *en = NULL;
    VARIANT_BOOL on_private = VARIANT_TRUE, on_public = VARIANT_TRUE;
    HRESULT init = E_FAIL;
    int n = 0;
    memset(fw, 0, sizeof(*fw));
    if (!co_init || !co_create || !co_uninit || !sys_free || !var_clear) {
        snprintf(fw->error, sizeof(fw->error), "Windows Firewall settings cannot be read here.");
        goto out;
    }
    init = co_init(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(co_create(&clsid, NULL, CLSCTX_INPROC_SERVER, &iid_policy, (void **)&policy)) || !policy) {
        snprintf(fw->error, sizeof(fw->error), "Windows Firewall did not answer.");
        goto out;
    }
    INetFwPolicy2_get_CurrentProfileTypes(policy, &fw->current);
    INetFwPolicy2_get_FirewallEnabled(policy, NET_FW_PROFILE2_PRIVATE, &on_private);
    INetFwPolicy2_get_FirewallEnabled(policy, NET_FW_PROFILE2_PUBLIC, &on_public);
    if (SUCCEEDED(INetFwPolicy2_get_Rules(policy, &list)) && list && SUCCEEDED(INetFwRules_get__NewEnum(list, &unk)) && unk &&
        SUCCEEDED(unk->lpVtbl->QueryInterface(unk, &iid_enum, (void **)&en)) && en) {
        VARIANT v;
        ULONG got = 0;
        memset(&v, 0, sizeof(v));
        while (en->lpVtbl->Next(en, 1, &v, &got) == S_OK && got == 1) {
            INetFwRule *rule = NULL;
            fw->rules_seen++;
            if ((v.vt == VT_DISPATCH || v.vt == VT_UNKNOWN) && v.punkVal &&
                SUCCEEDED(v.punkVal->lpVtbl->QueryInterface(v.punkVal, &iid_rule, (void **)&rule)) && rule) {
                struct lh_fw_rule t;
                BSTR app = NULL, ports = NULL;
                VARIANT_BOOL enabled = VARIANT_FALSE;
                NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_OUT;
                NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
                LONG protocol = 0, profiles = 0;
                memset(&t, 0, sizeof(t));
                INetFwRule_get_ApplicationName(rule, &app);
                INetFwRule_get_LocalPorts(rule, &ports);
                INetFwRule_get_Enabled(rule, &enabled);
                INetFwRule_get_Direction(rule, &dir);
                INetFwRule_get_Action(rule, &action);
                INetFwRule_get_Protocol(rule, &protocol);
                INetFwRule_get_Profiles(rule, &profiles);
                lhw_bstr(app, t.app, sizeof(t.app));
                lhw_bstr(ports, t.ports, sizeof(t.ports));
                t.enabled = enabled != VARIANT_FALSE;
                t.inbound = dir == NET_FW_RULE_DIR_IN;
                t.allow = action == NET_FW_ACTION_ALLOW;
                t.protocol = (int)protocol;
                t.profiles = profiles;
                /* keep only rules that can concern the game: its own, or every program's on a listed port */
                if (t.enabled && t.inbound && (lh_path_same(t.app, exe) || (!t.app[0] && t.ports[0] && strcmp(t.ports, "*") &&
                    lh_port_in(t.ports, port))) && n < LHW_FW_RULES)
                    rules[n++] = t;
                if (app) sys_free(app);
                if (ports) sys_free(ports);
                INetFwRule_Release(rule);
            }
            var_clear(&v);
        }
        fw->ok = 1;
    } else {
        snprintf(fw->error, sizeof(fw->error), "Could not read the Windows Firewall rules.");
    }
    fw->rules_kept = n;
    fw->private_verdict = lh_fw_eval(rules, n, exe, port, LH_FW_PRIVATE, on_private != VARIANT_FALSE);
    fw->public_verdict = lh_fw_eval(rules, n, exe, port, LH_FW_PUBLIC, on_public != VARIANT_FALSE);
out:
    if (en) en->lpVtbl->Release(en);
    if (unk) unk->lpVtbl->Release(unk);
    if (list) INetFwRules_Release(list);
    if (policy) INetFwPolicy2_Release(policy);
    if (SUCCEEDED(init) && co_uninit) co_uninit();
    if (ole) FreeLibrary(ole);
    if (aut) FreeLibrary(aut);
    return fw->ok;
}
#endif
