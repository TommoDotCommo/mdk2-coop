/* T0 for launcher_host_core.h: router wire formats, CGNAT, firewall verdicts, invite routes,
 * the lobby key and kicks, probe visibility, "Waiting for", and the pad from recorded XInput states.
 * Usage: launcher_host_test [fuzz rounds]. tools/test_launcher_host.py runs it with sanitisers and mutants. */
#include <stdio.h>
#include <stdlib.h>
#include "launcher_host_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint32_t ip4(int a, int b, int c, int d) { return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | (uint32_t)d; }

static void test_natpmp(void)
{
    uint8_t b[16], r[16] = { 0, 129, 0, 0, 0, 0, 0, 9, 0x6c, 0xfc, 0x6c, 0xfc, 0, 0, 0x0e, 0x10 };
    unsigned result, mapped;
    uint32_t life, ext;
    CHECK(lh_natpmp_addr_req(b) == 2 && b[0] == 0 && b[1] == 0);
    CHECK(lh_natpmp_map_req(b, 27900, 27900, 3600) == 12);
    CHECK(b[1] == 1 && lh_rd16(b + 4) == 27900 && lh_rd16(b + 6) == 27900 && lh_rd32(b + 8) == 3600);
    CHECK(lh_natpmp_map_req(b, 27900, 0, 0) == 12 && lh_rd32(b + 8) == 0);      /* delete */
    CHECK(lh_natpmp_map_reply(r, 16, 27900, &result, &mapped, &life) && result == 0 && mapped == 27900 && life == 3600);
    CHECK(!lh_natpmp_map_reply(r, 16, 27901, &result, &mapped, &life));          /* another port's answer */
    r[1] = 130;
    CHECK(!lh_natpmp_map_reply(r, 16, 27900, &result, &mapped, &life));
    {
        uint8_t a[12] = { 0, 128, 0, 0, 0, 0, 0, 1, 100, 72, 3, 4 };
        CHECK(lh_natpmp_addr_reply(a, 12, &result, &ext) && result == 0 && ext == ip4(100, 72, 3, 4));
        CHECK(lh_wan_class(ext) == LH_WAN_CGNAT);
        CHECK(!lh_natpmp_addr_reply(a, 11, &result, &ext));
    }
}

static void test_wan(void)
{
    CHECK(lh_wan_class(ip4(100, 64, 0, 1)) == LH_WAN_CGNAT);
    CHECK(lh_wan_class(ip4(100, 127, 255, 254)) == LH_WAN_CGNAT);
    CHECK(lh_wan_class(ip4(100, 128, 0, 1)) == LH_WAN_PUBLIC);
    CHECK(lh_wan_class(ip4(100, 63, 255, 255)) == LH_WAN_PUBLIC);
    CHECK(lh_wan_class(ip4(10, 1, 2, 3)) == LH_WAN_PRIVATE);
    CHECK(lh_wan_class(ip4(172, 20, 0, 1)) == LH_WAN_PRIVATE);
    CHECK(lh_wan_class(ip4(172, 32, 0, 1)) == LH_WAN_PUBLIC);
    CHECK(lh_wan_class(ip4(192, 168, 0, 1)) == LH_WAN_PRIVATE);
    CHECK(lh_wan_class(ip4(169, 254, 0, 1)) == LH_WAN_PRIVATE);
    CHECK(lh_wan_class(ip4(203, 0, 113, 9)) == LH_WAN_PUBLIC);
    CHECK(lh_wan_class(0) == LH_WAN_UNKNOWN);
    CHECK(lh_wan_class(ip4(127, 0, 0, 1)) == LH_WAN_UNKNOWN);
}

static void test_upnp(void)
{
    static const char ssdp[] = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                               "Location: http://192.168.1.1:5000/rootDesc.xml\r\nSERVER: test\r\n\r\n";
    static const char desc_rel[] = "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
        "<controlURL>/l3f</controlURL></service></serviceList><deviceList><device><serviceList><service>"
        "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>ctl/IPConn</controlURL>"
        "</service></serviceList></device></deviceList></device></root>";
    static const char desc_ppp[] = "<root><URLBase>http://192.168.1.254:49000</URLBase><service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
        "<controlURL>/upnp/control/WANPPPConn1</controlURL></service></root>";
    static const char desc_abs[] = "<root><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:2</serviceType>"
        "<controlURL>http://10.0.0.1:1780/ctl</controlURL></service></root>";
    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\n\r\n<s:Envelope><s:Body><u:GetExternalIPAddressResponse>"
        "<NewExternalIPAddress>100.80.1.2</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>";
    static const char fault[] = "HTTP/1.1 500 Internal Server Error\r\n\r\n<s:Envelope><s:Body><s:Fault><detail><UPnPError>"
        "<errorCode>718</errorCode><errorDescription>ConflictInMappingEntry</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
    struct lh_url u, bad;
    struct lh_igd g;
    struct lh_soap_answer a;
    char req[2048];
    CHECK(lh_ssdp_search(req, sizeof(req), "239.255.255.250") > 0 && strstr(req, "InternetGatewayDevice:1"));
    CHECK(lh_ssdp_location(ssdp, sizeof(ssdp) - 1, &u) && u.ip == ip4(192, 168, 1, 1) && u.port == 5000 && !strcmp(u.path, "/rootDesc.xml"));
    CHECK(!lh_ssdp_location("HTTP/1.1 404 Not Found\r\nLOCATION: http://1.2.3.4/x\r\n\r\n", 48, &bad));
    CHECK(!lh_parse_url("https://1.2.3.4/x", 17, &bad));
    CHECK(!lh_parse_url("http://router.local/x", 21, &bad));
    CHECK(!lh_parse_url("http://1.2.3.4:0/x", 18, &bad));
    CHECK(lh_igd_parse(desc_rel, sizeof(desc_rel) - 1, &u, &g) && g.control.ip == u.ip && g.control.port == 5000 &&
          !strcmp(g.control.path, "/ctl/IPConn") && strstr(g.service, "WANIPConnection:1"));
    CHECK(lh_igd_parse(desc_ppp, sizeof(desc_ppp) - 1, &u, &g) && g.control.ip == ip4(192, 168, 1, 254) && g.control.port == 49000 &&
          strstr(g.service, "WANPPPConnection:1"));
    CHECK(lh_igd_parse(desc_abs, sizeof(desc_abs) - 1, &u, &g) && g.control.ip == ip4(10, 0, 0, 1) && g.control.port == 1780 &&
          strstr(g.service, "WANIPConnection:2"));
    CHECK(!lh_igd_parse("<root/>", 7, &u, &g));
    lh_igd_parse(desc_rel, sizeof(desc_rel) - 1, &u, &g);
    CHECK(lh_soap(req, sizeof(req), &g, LH_SOAP_ADD, 27900, "192.168.1.20", 3600) > 0 && strstr(req, "POST /ctl/IPConn HTTP/1.1") &&
          strstr(req, "#AddPortMapping\"") && strstr(req, "<NewExternalPort>27900</NewExternalPort>") &&
          strstr(req, "<NewInternalClient>192.168.1.20</NewInternalClient>") && strstr(req, "<NewProtocol>UDP</NewProtocol>"));
    {   /* Content-Length is the body's */
        const char *body = strstr(req, "\r\n\r\n") + 4, *cl = strstr(req, "CONTENT-LENGTH: ");
        CHECK(cl && atoi(cl + 16) == (int)strlen(body));
    }
    CHECK(lh_soap(req, sizeof(req), &g, LH_SOAP_DELETE, 27900, "", 0) > 0 && strstr(req, "#DeletePortMapping\"") && !strstr(req, "NewInternalClient"));
    CHECK(lh_soap(req, 64, &g, LH_SOAP_ADD, 27900, "192.168.1.20", 3600) == 0);
    CHECK(lh_soap_parse(ok, sizeof(ok) - 1, &a) && a.status == 200 && a.ext_ip == ip4(100, 80, 1, 2) && !a.error);
    CHECK(lh_soap_parse(fault, sizeof(fault) - 1, &a) && a.status == 500 && a.error == 718 && !strcmp(a.description, "ConflictInMappingEntry"));
    CHECK(!lh_soap_parse("garbage", 7, &a));
}

static void test_firewall(void)
{
    static const char exe[] = "C:\\Games\\MDK2 HD\\mdk2hd.exe";
    struct lh_fw_rule r[6];
    char line[160];
    memset(r, 0, sizeof(r));
    r[0].enabled = 1; r[0].inbound = 1; r[0].allow = 1; r[0].protocol = 17; r[0].profiles = LH_FW_PRIVATE;
    strcpy(r[0].app, "c:/games/mdk2 hd/MDK2HD.EXE");
    CHECK(lh_fw_eval(r, 1, exe, 27900, LH_FW_PRIVATE, 1) == LH_FW_ALLOWED);
    CHECK(lh_fw_eval(r, 1, exe, 27900, LH_FW_PUBLIC, 1) == LH_FW_NO_RULE);
    CHECK(lh_fw_eval(r, 1, exe, 27900, LH_FW_PUBLIC, 0) == LH_FW_OFF);
    CHECK(lh_fw_eval(r, 1, "C:\\other\\game.exe", 27900, LH_FW_PRIVATE, 1) == LH_FW_NO_RULE);
    /* Windows' own prompt makes a block rule for the Public profile: it wins over an allow rule */
    r[1] = r[0]; r[1].allow = 0; r[1].profiles = 0x7fffffff;
    CHECK(lh_fw_eval(r, 2, exe, 27900, LH_FW_PRIVATE, 1) == LH_FW_BLOCKED);
    r[1].enabled = 0;
    CHECK(lh_fw_eval(r, 2, exe, 27900, LH_FW_PRIVATE, 1) == LH_FW_ALLOWED);
    /* TCP only does not let UDP in; outbound rules do not count */
    r[2] = r[0]; r[2].protocol = 6; r[2].profiles = LH_FW_PUBLIC;
    r[3] = r[0]; r[3].inbound = 0; r[3].profiles = LH_FW_PUBLIC;
    CHECK(lh_fw_eval(r, 4, exe, 27900, LH_FW_PUBLIC, 1) == LH_FW_NO_RULE);
    /* a port rule for every program */
    r[4].enabled = 1; r[4].inbound = 1; r[4].allow = 1; r[4].protocol = 256; r[4].profiles = LH_FW_PUBLIC;
    strcpy(r[4].ports, "80, 27899-27903");
    CHECK(lh_fw_eval(r, 5, exe, 27900, LH_FW_PUBLIC, 1) == LH_FW_ALLOWED);
    CHECK(lh_fw_eval(r, 5, exe, 27904, LH_FW_PUBLIC, 1) == LH_FW_NO_RULE);
    CHECK(lh_port_in("*", 1) && lh_port_in("", 1) && lh_port_in("Any", 9) && !lh_port_in("27901", 27900) && lh_port_in("1,27900", 27900));
    CHECK(lh_fw_line(LH_FW_ALLOWED, LH_FW_ALLOWED, line, sizeof(line)) && strstr(line, "Private networks: allowed"));
    CHECK(!lh_fw_line(LH_FW_ALLOWED, LH_FW_NO_RULE, line, sizeof(line)) && strstr(line, "Public networks: no rule"));
    CHECK(!lh_fw_line(LH_FW_BLOCKED, LH_FW_OFF, line, sizeof(line)) && strstr(line, "blocked"));
}

static void test_invite(void)
{
    struct lh_addr a[4] = { { ip4(169, 254, 3, 4), 16 }, { ip4(100, 101, 102, 103), 32 }, { ip4(192, 168, 1, 20), 24 }, { ip4(8, 8, 4, 4), 24 } };
    struct ci_invite v, back;
    char code[CI_CODE_MAX], msg[256];
    const char *other;
    memset(&v, 0, sizeof(v));
    CHECK(lh_invite_routes(&v, a, 4, ip4(100, 101, 102, 103), ip4(203, 0, 113, 7), 1) == 4);
    CHECK(v.route[0].kind == CI_LAN && v.route[0].ip[0] == 192 && v.route[0].ip[3] == 20);
    CHECK(v.route[1].kind == CI_LAN && v.route[1].ip[0] == 169);
    CHECK(v.route[2].kind == CI_VPN && v.route[2].ip[0] == 100 && v.route[2].ip[3] == 103);
    CHECK(v.route[3].kind == CI_INTERNET && v.route[3].ip[0] == 203);
    CHECK(lh_invite_routes(&v, a, 4, 0, ip4(203, 0, 113, 7), 0) == 2);          /* internet check failed: left out */
    CHECK(lh_invite_routes(&v, a, 4, 0, ip4(100, 70, 0, 1), 1) == 2);           /* CGNAT address is never "internet" */
    v.flags = CI_FLAG_INVITE_ONLY | CI_FLAG_STEAM; v.secret = 0xdeadbeef; v.port = 27916; v.steam_lobby = 109775241234567890ull;
    lh_invite_routes(&v, a, 4, ip4(100, 101, 102, 103), ip4(203, 0, 113, 7), 1);
    CHECK(ci_encode(&v, "MDK2", code, sizeof(code)) > 0);
    CHECK(ci_decode(code, strlen(code), "MDK2", &back, &other) == CI_OK && back.secret == v.secret && back.port == 27916 && back.n == 4 &&
          back.steam_lobby == v.steam_lobby && back.flags == v.flags && !memcmp(back.route, v.route, sizeof(v.route)));
    CHECK(ci_message("Fox", "MDK2", "tommodotcommo.github.io/join", code, msg, sizeof(msg)) > 0 &&
          !strncmp(msg, "Fox is hosting MDK2. Join: https://tommodotcommo.github.io/join/#MDK2-", 69));
}

static void test_lobby(void)
{
    struct lh_hello h;
    struct lh_bans bans;
    char line[200];
    memset(&bans, 0, sizeof(bans));
    CHECK(lh_hello_parse("MDKL H 1a2b 1 30 12345 v0.4.0|Fox", &h) && h.id == 0x1a2b && h.ready == 1 && h.rtt == 30 &&
          !h.has_key && !strcmp(h.version, "v0.4.0") && !strcmp(h.name, "Fox"));
    CHECK(lh_hello_write(line, sizeof(line), 0x1a2b, 0, 5, 99, "v0.4.0", 1, 0xcafe0001u, "Fox Two") > 0);
    CHECK(lh_hello_parse(line, &h) && h.has_key && h.key == 0xcafe0001u && !strcmp(h.version, "v0.4.0") && !strcmp(h.name, "Fox Two"));
    CHECK(!lh_hello_parse("MDKL H zz|Fox", &h) && !lh_hello_parse("MDKL H 1 1 1 1 v1", &h));
    /* open lobby: no key needed */
    lh_hello_parse("MDKL H 1a2b 1 30 12345 v0.4.0|Fox", &h);
    CHECK(lh_admit(&bans, 0, 0x1234, &h, ip4(192, 168, 1, 5), 50000, 1, 16) == LH_ADMIT);
    /* invite only: the code's secret, nothing else */
    CHECK(lh_admit(&bans, 1, 0x1234, &h, ip4(192, 168, 1, 5), 50000, 1, 16) == LH_REFUSE_INVITE);
    h.has_key = 1; h.key = 0x1235;
    CHECK(lh_admit(&bans, 1, 0x1234, &h, ip4(192, 168, 1, 5), 50000, 1, 16) == LH_REFUSE_INVITE);
    h.key = 0x1234;
    CHECK(lh_admit(&bans, 1, 0x1234, &h, ip4(192, 168, 1, 5), 50000, 1, 16) == LH_ADMIT);
    h.has_key = 0; h.key = 0;
    CHECK(lh_admit(&bans, 1, 0, &h, ip4(192, 168, 1, 5), 50000, 1, 16) == LH_REFUSE_INVITE);   /* no key is not key 0 */
    /* kicked: refused for this session by id, and by address with a new id */
    h.has_key = 1; h.key = 0x1234;
    lh_ban(&bans, h.id, ip4(192, 168, 1, 5), 50000);
    CHECK(lh_admit(&bans, 1, 0x1234, &h, ip4(192, 168, 1, 5), 50000, 1, 16) == LH_REFUSE_KICKED);
    h.id = 0x9999;
    CHECK(lh_admit(&bans, 0, 0x1234, &h, ip4(192, 168, 1, 5), 50000, 1, 16) == LH_REFUSE_KICKED);
    CHECK(lh_admit(&bans, 0, 0x1234, &h, ip4(192, 168, 1, 6), 50000, 1, 16) == LH_ADMIT);
    CHECK(lh_admit(&bans, 0, 0x1234, &h, ip4(192, 168, 1, 5), 50001, 1, 16) == LH_ADMIT);    /* same PC, another player */
    CHECK(lh_admit(&bans, 0, 0x1234, &h, ip4(192, 168, 1, 6), 50000, 16, 16) == LH_REFUSE_FULL);
    CHECK(lh_refusal_parse("MDKL K 2", 8) == LH_REFUSE_INVITE && !lh_refusal_parse("MDKL K 9", 8) && !lh_refusal_parse("MDKL R 2", 8));
    CHECK(strstr(lh_refusal_text(LH_REFUSE_INVITE), "invite only") != NULL);
    /* B9: invite only is not listed by the v1 probe, still answers v2 (flagged, nameless) */
    CHECK(lh_probe_allowed(0, LH_PROBE_V1) && lh_probe_allowed(0, LH_PROBE_V2));
    CHECK(!lh_probe_allowed(1, LH_PROBE_V1) && lh_probe_allowed(1, LH_PROBE_V2));
    {
        const char *names[5] = { "Host", "Fox", "Bob", "Ann", "Cy" };
        int ready[5] = { 1, 0, 1, 1, 1 };
        CHECK(lh_waiting(line, sizeof(line), names, ready, 5) == 1 && !strcmp(line, "Waiting for Fox"));
        ready[2] = 0;
        CHECK(lh_waiting(line, sizeof(line), names, ready, 5) == 2 && !strcmp(line, "Waiting for Fox and Bob"));
        ready[3] = 0;
        CHECK(lh_waiting(line, sizeof(line), names, ready, 5) == 3 && !strcmp(line, "Waiting for Fox, Bob and Ann"));
        ready[4] = 0;
        CHECK(lh_waiting(line, sizeof(line), names, ready, 5) == 4 && !strcmp(line, "Waiting for Fox, Bob and 2 more"));
        ready[1] = ready[2] = ready[3] = ready[4] = 1;
        CHECK(lh_waiting(line, sizeof(line), names, ready, 5) == 0 && !line[0]);
    }
}

/* Recorded pad states: (time ms, buttons, left stick x, y) and the actions expected after each. */
struct pad_sample { uint32_t t; unsigned buttons; int lx, ly; int expect[4]; };
static void pad_run(const char *name, const struct pad_sample *s, int n)
{
    struct lh_pad p;
    int i, k;
    memset(&p, 0, sizeof(p));
    for (i = 0; i < n; i++) {
        int out[8], got = lh_pad_step(&p, s[i].buttons, s[i].lx, s[i].ly, s[i].t, out, 8), want = 0, ok;
        while (want < 4 && s[i].expect[want]) want++;
        ok = got == want;
        for (k = 0; ok && k < want; k++) ok = out[k] == s[i].expect[k];
        checks++;
        if (!ok) { fails++; printf("FAIL pad %s sample %d: got %d actions (first %d), want %d\n", name, i, got, got ? out[0] : 0, want); }
    }
}
static void test_pad(void)
{
    /* Play page to Start: down the tiles with the stick, a held D-pad repeats, A picks, RB / LB tabs, Start plays */
    static const struct pad_sample nav[] = {
        { 0, 0, 0, 0, { 0 } },
        { 16, 0, 0, -30000, { LH_ACT_DOWN } },
        { 32, 0, 0, -30000, { 0 } },
        { 48, 0, 0, -8000, { 0 } },                                  /* back inside the dead zone */
        { 64, LH_PAD_DPAD_DOWN, 0, 0, { LH_ACT_DOWN } },
        { 400, LH_PAD_DPAD_DOWN, 0, 0, { 0 } },
        { 470, LH_PAD_DPAD_DOWN, 0, 0, { LH_ACT_DOWN } },             /* repeat after 400 ms */
        { 560, LH_PAD_DPAD_DOWN, 0, 0, { 0 } },
        { 600, LH_PAD_DPAD_DOWN, 0, 0, { LH_ACT_DOWN } },             /* then every 120 ms */
        { 620, 0, 0, 0, { 0 } },
        { 640, LH_PAD_A, 0, 0, { LH_ACT_CONFIRM } },
        { 660, LH_PAD_A, 0, 0, { 0 } },                               /* held A acts once */
        { 680, LH_PAD_RB, 0, 0, { LH_ACT_TAB_NEXT } },
        { 700, LH_PAD_LB, 0, 0, { LH_ACT_TAB_PREV } },
        { 720, LH_PAD_Y, 0, 0, { LH_ACT_Y_DOWN } },
        { 740, 0, 0, 0, { LH_ACT_Y_UP } },
        { 760, LH_PAD_X | LH_PAD_B, 0, 0, { LH_ACT_BACK, LH_ACT_X } },
        { 780, 0, 30000, 0, { LH_ACT_RIGHT } },
        { 800, LH_PAD_START, 0, 0, { LH_ACT_PRIMARY } },
    };
    pad_run("navigation", nav, (int)(sizeof(nav) / sizeof(nav[0])));
    CHECK(lh_tab_step(0, 4, LH_ACT_TAB_NEXT) == 1 && lh_tab_step(3, 4, LH_ACT_TAB_NEXT) == 0);
    CHECK(lh_tab_step(0, 4, LH_ACT_TAB_PREV) == 3 && lh_tab_step(2, 4, LH_ACT_CONFIRM) == 2);
}

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void fuzz(long rounds)
{
    static char buf[1025];
    long r;
    for (r = 0; r < rounds; r++) {
        size_t n = rnd() % (sizeof(buf) - 1), i;
        struct lh_url u;
        struct lh_igd g;
        struct lh_soap_answer a;
        struct lh_hello h;
        unsigned x, y;
        uint32_t z;
        static const char *seeds[] = { "HTTP/1.1 200 OK\r\nLOCATION: http://", "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>",
                                       "HTTP/1.1 500 x\r\n\r\n<errorCode>", "MDKL H 1 1 1 1 v|", "MDKL K " };
        for (i = 0; i < n; i++) buf[i] = rnd() & 1 ? (char)rnd() : "0123456789./:<>-|abcHTTP"[rnd() % 24];
        if (rnd() & 1) { const char *s = seeds[rnd() % 5]; size_t k = strlen(s); if (k < n) memcpy(buf, s, k); }
        lh_ssdp_location(buf, n, &u);
        lh_parse_url(buf, n, &u);
        memset(&u, 0, sizeof(u)); u.port = 80; strcpy(u.path, "/");
        lh_igd_parse(buf, n, &u, &g);
        lh_soap_parse(buf, n, &a);
        lh_natpmp_map_reply((uint8_t *)buf, n % 20, 27900, &x, &y, &z);
        lh_natpmp_addr_reply((uint8_t *)buf, n % 20, &x, &z);
        lh_refusal_parse(buf, n);
        buf[n] = 0;
        lh_hello_parse(buf, &h);
        lh_port_in(buf, 27900);
    }
}

int main(int argc, char **argv)
{
    long rounds = argc > 1 ? atol(argv[1]) : 2000;
    test_natpmp();
    test_wan();
    test_upnp();
    test_firewall();
    test_invite();
    test_lobby();
    test_pad();
    fuzz(rounds);
    printf("%s: launcher host core, %d checks, %ld fuzz rounds, %d failed\n", fails ? "FAIL" : "PASS", checks, rounds, fails);
    return fails ? 1 : 0;
}
