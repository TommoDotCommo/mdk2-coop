/* Windows T0 for launcher_host_win.h: the router code against a fake UPnP IGD (SSDP + HTTP/SOAP) and a
 * fake NAT-PMP responder on 127.0.0.1, and a read-only pass over the real Windows Firewall rules.
 * Usage: launcher_host_win_test.exe. Exit 0 when every case passes. MIT licence. */
#include "launcher_host_win.h"
#include <stdlib.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); } } while (0)

/* What the fakes know: one UPnP and one NAT-PMP table entry, and how they behave. */
static struct {
    volatile LONG stop;
    SOCKET ssdp, http, natpmp;
    unsigned http_port;
    volatile int ssdp_silent, upnp_refuse_lease, natpmp_silent;
    uint32_t ext_ip;
    CRITICAL_SECTION lock;
    int upnp_mapped, upnp_adds, upnp_deletes; unsigned upnp_port, upnp_lease; char upnp_client[32];
    int natpmp_mapped, natpmp_adds, natpmp_deletes; unsigned natpmp_port; uint32_t natpmp_life;
} fake;

static SOCKET bind_lo(int type, unsigned *port)
{
    SOCKET s = socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
    struct sockaddr_in a;
    int len = sizeof(a);
    lhw_sin(&a, 0x7f000001u, 0);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    getsockname(s, (struct sockaddr *)&a, &len);
    *port = ntohs(a.sin_port);
    if (type == SOCK_STREAM) listen(s, 8);
    return s;
}
static int tag_uint(const char *s, const char *tag, unsigned *v)
{
    char t[64];
    if (!lh_xml_text(s, strlen(s), tag, t, sizeof(t))) return 0;
    *v = (unsigned)strtoul(t, NULL, 10);
    return 1;
}
static DWORD WINAPI ssdp_loop(void *unused)
{
    char buf[2048], reply[256];
    struct sockaddr_in from;
    while (!fake.stop) {
        int fl = sizeof(from), n;
        if (!lhw_wait(fake.ssdp, 0, 50)) continue;
        n = recvfrom(fake.ssdp, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0 || fake.ssdp_silent || strncmp(buf, "M-SEARCH", 8)) continue;
        n = snprintf(reply, sizeof(reply), "HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                     "LOCATION: http://127.0.0.1:%u/desc.xml\r\n\r\n", fake.http_port);
        sendto(fake.ssdp, reply, n, 0, (struct sockaddr *)&from, fl);
    }
    return 0;
}
static DWORD WINAPI http_loop(void *unused)
{
    static const char desc[] = "<?xml version=\"1.0\"?><root><device><deviceList><device><serviceList><service>"
        "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/ctl</controlURL>"
        "</service></serviceList></device></deviceList></device></root>";
    static char buf[16384], out[4096];
    while (!fake.stop) {
        SOCKET c;
        int got = 0, n, want = -1;
        char body[1024];
        if (!lhw_wait(fake.http, 0, 50) || (c = accept(fake.http, NULL, NULL)) == INVALID_SOCKET) continue;
        while (got < (int)sizeof(buf) - 1 && lhw_wait(c, 0, 2000)) {
            const char *end, *cl;
            n = recv(c, buf + got, sizeof(buf) - 1 - got, 0);
            if (n <= 0) break;
            got += n; buf[got] = 0;
            if ((end = strstr(buf, "\r\n\r\n"))) {
                cl = lh_find(buf, (size_t)(end - buf), "CONTENT-LENGTH:");
                want = (int)(end + 4 - buf) + (cl ? atoi(cl + 15) : 0);
                if (got >= want) break;
            }
        }
        buf[got] = 0;
        body[0] = 0;
        EnterCriticalSection(&fake.lock);
        if (!strncmp(buf, "GET /desc.xml", 13)) {
            n = snprintf(out, sizeof(out), "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: %d\r\n\r\n%s", (int)strlen(desc), desc);
        } else if (strstr(buf, "#AddPortMapping\"")) {
            unsigned port = 0, lease = 99;
            tag_uint(buf, "NewExternalPort", &port);
            tag_uint(buf, "NewLeaseDuration", &lease);
            if (fake.upnp_refuse_lease && lease) {
                snprintf(body, sizeof(body), "<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>725</errorCode>"
                         "<errorDescription>OnlyPermanentLeasesSupported</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>");
                n = snprintf(out, sizeof(out), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: %d\r\n\r\n%s", (int)strlen(body), body);
            } else {
                fake.upnp_mapped = 1; fake.upnp_adds++; fake.upnp_port = port; fake.upnp_lease = lease;
                lh_xml_text(buf, strlen(buf), "NewInternalClient", fake.upnp_client, sizeof(fake.upnp_client));
                n = snprintf(out, sizeof(out), "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
            }
        } else if (strstr(buf, "#DeletePortMapping\"")) {
            unsigned port = 0;
            tag_uint(buf, "NewExternalPort", &port);
            if (port == fake.upnp_port) fake.upnp_mapped = 0;
            fake.upnp_deletes++;
            n = snprintf(out, sizeof(out), "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        } else if (strstr(buf, "#GetExternalIPAddress\"")) {
            snprintf(body, sizeof(body), "<s:Envelope><s:Body><u:GetExternalIPAddressResponse><NewExternalIPAddress>%u.%u.%u.%u"
                     "</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>",
                     fake.ext_ip >> 24, fake.ext_ip >> 16 & 255, fake.ext_ip >> 8 & 255, fake.ext_ip & 255);
            n = snprintf(out, sizeof(out), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%s", (int)strlen(body), body);
        } else {
            n = snprintf(out, sizeof(out), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        }
        LeaveCriticalSection(&fake.lock);
        send(c, out, n, 0);
        shutdown(c, SD_SEND);
        closesocket(c);
    }
    return 0;
}
static DWORD WINAPI natpmp_loop(void *unused)
{
    uint8_t q[64], a[16];
    struct sockaddr_in from;
    while (!fake.stop) {
        int fl = sizeof(from), n;
        if (!lhw_wait(fake.natpmp, 0, 50)) continue;
        n = recvfrom(fake.natpmp, (char *)q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0 || fake.natpmp_silent) continue;
        memset(a, 0, sizeof(a));
        EnterCriticalSection(&fake.lock);
        if (n == 2 && q[0] == 0 && q[1] == 0) {
            a[1] = 128; lh_be32(a + 8, fake.ext_ip);
            n = 12;
        } else if (n == 12 && q[1] == 1) {
            uint32_t life = lh_rd32(q + 8);
            unsigned port = lh_rd16(q + 4);
            if (life) { fake.natpmp_mapped = 1; fake.natpmp_adds++; fake.natpmp_port = port; fake.natpmp_life = life; }
            else if (port == fake.natpmp_port) { fake.natpmp_mapped = 0; fake.natpmp_deletes++; }
            a[1] = 129; memcpy(a + 8, q + 4, 2); memcpy(a + 10, life ? q + 4 : q + 6, 2); lh_be32(a + 12, life);
            n = 16;
        } else {
            n = 0;
        }
        LeaveCriticalSection(&fake.lock);
        if (n) sendto(fake.natpmp, (const char *)a, n, 0, (struct sockaddr *)&from, fl);
    }
    return 0;
}

/* the game's side of the hand-off: lhw_router_keep on its own thread until the game "quits" */
static struct { struct lhw_router r; const struct lhw_router_cfg *cfg; volatile LONG end; } kept;
static DWORD WINAPI keep_loop(void *unused)
{
    lhw_router_keep(&kept.r, kept.cfg, &kept.end, 100);
    return 0;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    int adds;
    unsigned ssdp_port, natpmp_port, silent_port;
    HANDLE threads[3];
    struct lhw_router_cfg cfg;
    struct lhw_router r;
    SOCKET silent;
    struct lhw_fw fw;
    char exe[MAX_PATH], line[160];

    WSAStartup(MAKEWORD(2, 2), &wsa);
    InitializeCriticalSection(&fake.lock);
    fake.ssdp = bind_lo(SOCK_DGRAM, &ssdp_port);
    fake.http = bind_lo(SOCK_STREAM, &fake.http_port);
    fake.natpmp = bind_lo(SOCK_DGRAM, &natpmp_port);
    silent = bind_lo(SOCK_DGRAM, &silent_port);             /* a "router" that never answers */
    threads[0] = CreateThread(NULL, 0, ssdp_loop, NULL, 0, NULL);
    threads[1] = CreateThread(NULL, 0, http_loop, NULL, 0, NULL);
    threads[2] = CreateThread(NULL, 0, natpmp_loop, NULL, 0, NULL);
    memset(&cfg, 0, sizeof(cfg));
    cfg.ssdp_ip = cfg.natpmp_ip = 0x7f000001u;
    cfg.ssdp_port = ssdp_port;
    cfg.natpmp_port = natpmp_port;
    cfg.timeout_ms = 600;

    /* 1. UPnP: mapped for this PC, the WAN address read, renewed at half the lease, removed on close */
    fake.ext_ip = 0xcb007105u;                              /* 203.0.113.5 */
    cfg.lease = 2;
    CHECK(lhw_router_open(&r, &cfg, 27916) && r.method == LHW_UPNP);
    CHECK(fake.upnp_mapped && fake.upnp_port == 27916 && fake.upnp_lease == 2 && !strcmp(fake.upnp_client, "127.0.0.1"));
    CHECK(r.ext_ip == 0xcb007105u && r.wan == LH_WAN_PUBLIC && strstr(r.said, "UPnP"));
    CHECK(!fake.natpmp_adds);                                /* NAT-PMP only when UPnP fails */
    CHECK(!lhw_router_renew(&r, &cfg, GetTickCount()));      /* not yet: half the lease */
    Sleep(1100);
    CHECK(lhw_router_renew(&r, &cfg, GetTickCount()) && fake.upnp_adds == 2);
    lhw_router_close(&r, &cfg);
    CHECK(!fake.upnp_mapped && fake.upnp_deletes == 1 && r.method == LHW_NONE);
    printf("upnp: %s\n", r.said);

    /* 1b. handed to the game: renewed while it runs, removed when it quits */
    CHECK(lhw_router_open(&kept.r, &cfg, 27916) && fake.upnp_mapped);
    kept.cfg = &cfg;
    adds = fake.upnp_adds;
    {
        HANDLE t = CreateThread(NULL, 0, keep_loop, NULL, 0, NULL);
        Sleep(1400);
        CHECK(fake.upnp_mapped && fake.upnp_adds > adds);        /* the game runs: renewed, still mapped */
        InterlockedExchange(&kept.end, 1);                       /* the game quits */
        CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
        CloseHandle(t);
    }
    CHECK(!fake.upnp_mapped && fake.upnp_deletes == 2);

    /* 2. a router that keeps only permanent mappings (725): asked again with lease 0, never renewed */
    fake.upnp_refuse_lease = 1;
    cfg.lease = 0;
    CHECK(lhw_router_open(&r, &cfg, 27917) && r.method == LHW_UPNP && r.lease == 0 && fake.upnp_lease == 0);
    CHECK(!lhw_router_renew(&r, &cfg, GetTickCount() + 10000000u));
    lhw_router_close(&r, &cfg);
    CHECK(!fake.upnp_mapped);
    fake.upnp_refuse_lease = 0;

    /* 3. no UPnP: NAT-PMP maps; the WAN address is in 100.64.0.0/10 (CGNAT); removed on close */
    fake.ssdp_silent = 1;
    fake.ext_ip = 0x64480009u;                              /* 100.72.0.9 */
    CHECK(lhw_router_open(&r, &cfg, 27918) && r.method == LHW_NATPMP && fake.natpmp_mapped && fake.natpmp_port == 27918);
    CHECK(r.wan == LH_WAN_CGNAT && r.ext_ip == 0x64480009u && fake.natpmp_life == LH_LEASE_S && strstr(r.said, "NAT-PMP"));
    lhw_router_close(&r, &cfg);
    CHECK(!fake.natpmp_mapped && fake.natpmp_deletes == 1);
    printf("natpmp: %s\n", r.said);

    /* 4. nothing answers: no mapping, and the panel says so */
    fake.natpmp_silent = 1;
    cfg.natpmp_port = silent_port;
    CHECK(!lhw_router_open(&r, &cfg, 27919) && r.method == LHW_NONE && strstr(r.said, "No answer") && r.wan == LH_WAN_UNKNOWN);
    printf("silent: %s\n", r.said);
    lhw_router_close(&r, &cfg);

    /* 5. the firewall, read only, for this program: finishes and gives a verdict or a reason */
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    CHECK(lhw_firewall(exe, 27900, &fw) || fw.error[0]);
    lh_fw_line(fw.private_verdict, fw.public_verdict, line, sizeof(line));
    printf("firewall: ok=%d current=%ld rules=%d kept=%d %s %s\n", fw.ok, fw.current, fw.rules_seen, fw.rules_kept, line, fw.error);

    fake.stop = 1;
    WaitForMultipleObjects(3, threads, TRUE, 3000);
    closesocket(silent);
    printf("%s: launcher host (Windows), %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
