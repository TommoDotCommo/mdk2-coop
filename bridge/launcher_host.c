/* The launcher's hosting side: see launcher_host.h. The checks run on one worker thread per lobby and
 * publish rows under a lock; the UI thread reads them, answers probes and decides who may join.
 * Wire formats and decisions are in launcher_host_core.h (tested game-free). MIT licence. */
#include "bridge.h"
#include "pad.h"
#include "coop_discover_win.h"
#include "launcher_host_win.h"
#include "launcher_host.h"

struct lhost_settings lhost_cfg = { 1, 1, 0 };

void lhost_settings_load(const char *ini)
{
    lhost_cfg.upnp = GetPrivateProfileIntA("network", "upnp", 1, ini) != 0;
    lhost_cfg.internet_in_invite = GetPrivateProfileIntA("network", "internet_in_invite", 1, ini) != 0;
    lhost_cfg.invite_only = GetPrivateProfileIntA("network", "invite_only", 0, ini) != 0;
}

void lhost_settings_save(const char *ini)
{
    static const char *keys[] = { "upnp", "internet_in_invite", "invite_only" };
    int v[3], i;
    v[0] = lhost_cfg.upnp; v[1] = lhost_cfg.internet_in_invite; v[2] = lhost_cfg.invite_only;
    for (i = 0; i < 3; i++)
        WritePrivateProfileStringA("network", keys[i], v[i] ? "1" : "0", ini);
}

/* ---- the worker's findings ---- */
#define LHOST_ADDRS 8
enum { RT_OFF, RT_TRYING, RT_MAPPED, RT_FAILED };
enum { TS_CHECKING, TS_RUNNING, TS_ABSENT, TS_STOPPED };
static CRITICAL_SECTION lk;
static LONG lk_made;
static volatile LONG lobby_gen;     /* each lobby's worker publishes only while its lobby is the current one */
static struct {
    int open, steam, nlan, ts_state, router_state, lan_probe, fw_done;   /* lan_probe: 0 checking, 1 answered, 2 silent */
    unsigned port;
    uint32_t secret, ts_ip;
    struct lh_addr lan[LHOST_ADDRS];
    char ts_name[128];
    struct lhw_router router;
    struct lhw_fw fw;
    struct lh_bans bans;
    HANDLE thread, kept;            /* kept: the worker that renews the mapping for the game (lhost_close(1)) */
    volatile LONG stop, keep, game_end;
    int handed;                     /* the game started from this lobby */
} H;
static uint32_t join_key;           /* client: the invite's key for the host's game (lhost_set_join_key) */
static int join_has_key;

static void lock_make(void)
{
    if (InterlockedCompareExchange(&lk_made, 1, 0) == 0)
        InitializeCriticalSection(&lk);
}

static void ip_text(uint32_t ip, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

static uint32_t random_u32(void)
{
    typedef BOOLEAN (WINAPI *gen_fn)(PVOID, ULONG);
    HMODULE adv = lhw_system_dll("advapi32.dll");
    gen_fn gen = adv ? (gen_fn)(void (*)(void))GetProcAddress(adv, "SystemFunction036") : NULL;   /* RtlGenRandom */
    uint32_t v = 0;
    LARGE_INTEGER t;
    if (!gen || !gen(&v, sizeof(v))) {
        QueryPerformanceCounter(&t);
        v = (uint32_t)t.QuadPart * 2654435761u ^ GetTickCount() ^ (GetCurrentProcessId() << 16);
    }
    if (adv) FreeLibrary(adv);
    return v;
}

/* Every private or link-local IPv4 address, the one towards the internet first. */
static int lan_addresses(struct lh_addr *out, int max)
{
    struct cdw_adapter a[16];
    uint32_t main_ip = lhw_local_ip_to(0x08080808u);
    int n = cdw_adapters(a, 16), i, k = 0;
    if (main_ip && (lh_private_ip(main_ip) || (main_ip & 0xffff0000u) == 0xa9fe0000u) && k < max)
        out[k].ip = main_ip, out[k++].prefix = 24;
    for (i = 0; i < n && k < max; i++) {
        uint32_t ip = a[i].a.ip;
        if (ip == main_ip || !(lh_private_ip(ip) || (ip & 0xffff0000u) == 0xa9fe0000u)) continue;
        out[k].ip = ip; out[k++].prefix = a[i].a.prefix;
    }
    return k;
}

/* This PC's lobby, asked on its own LAN address: the lobby socket (UI thread) answers it. */
static int stopping(LONG gen) { return gen != lobby_gen || H.stop; }

static int probe_self(uint32_t ip, unsigned port, LONG gen)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in to, from;
    uint8_t q[CD_QUERY_LEN], b[CD_REPLY_MAX + 8];
    uint32_t nonce = random_u32();
    struct cd_reply rep;
    int k, ok = 0;
    if (s == INVALID_SOCKET) return 0;
    lhw_sin(&to, ip, port);
    cd_query(q, nonce, "MDK2", 0);
    for (k = 0; k < 3 && !ok && !stopping(gen); k++) {
        DWORD start = GetTickCount();
        sendto(s, (const char *)q, CD_QUERY_LEN, 0, (struct sockaddr *)&to, sizeof(to));
        while (!ok && lhw_left(start, 700) && lhw_wait(s, 0, lhw_left(start, 700))) {
            int fl = sizeof(from), n = recvfrom(s, (char *)b, sizeof(b), 0, (struct sockaddr *)&from, &fl);
            if (n > 0 && cd_parse_reply(b, (size_t)n, nonce, "MDK2", &rep)) ok = 1;
        }
    }
    closesocket(s);
    return ok;
}

static DWORD WINAPI worker(void *param)
{
    LONG gen = (LONG)(INT_PTR)param;
    struct lh_addr lan[LHOST_ADDRS] = { { 0, 0 } };
    struct lhw_router_cfg rcfg;
    struct lhw_router router;
    struct lhw_fw fw;
    char exe[MAX_PATH], *json;
    int n, mapped = 0;
    unsigned port = H.port;
    WSADATA wsa;

    WSAStartup(MAKEWORD(2, 2), &wsa);
    n = lan_addresses(lan, LHOST_ADDRS);
    EnterCriticalSection(&lk);
    if (gen == lobby_gen) {
        memcpy(H.lan, lan, sizeof(lan));
        H.nlan = n;
    }
    LeaveCriticalSection(&lk);
    {
        int ok = n > 0 && probe_self(lan[0].ip, port, gen);
        EnterCriticalSection(&lk);
        if (gen == lobby_gen) H.lan_probe = ok ? 1 : 2;
        LeaveCriticalSection(&lk);
        logmsg("host: same network: %d address(es), lobby %s", n, ok ? "answers on this PC's LAN address" : "did not answer");
    }
    if (!stopping(gen)) {
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        lhw_firewall(exe, port, &fw);
        EnterCriticalSection(&lk);
        if (gen == lobby_gen) {
            H.fw = fw;
            H.fw_done = 1;
        }
        LeaveCriticalSection(&lk);
        logmsg("host: firewall: read=%d private=%d public=%d (%d rules, %d for this game) %s", fw.ok, fw.private_verdict,
               fw.public_verdict, fw.rules_seen, fw.rules_kept, fw.error);
    }
    if (!stopping(gen) && (json = malloc(CD_JSON_MAX)) != NULL) {
        static struct cd_ts ts;
        int got = cdw_tailscale_status(json, CD_JSON_MAX, 2000), state;
        if (got == 0) state = TS_ABSENT;
        else if (got > 0 && cd_ts_parse(json, (size_t)got, &ts) && ts.running && ts.self.ip) state = TS_RUNNING;
        else state = TS_STOPPED;
        free(json);
        EnterCriticalSection(&lk);
        if (gen == lobby_gen) H.ts_state = state;
        if (gen == lobby_gen && state == TS_RUNNING) {
            size_t len;
            H.ts_ip = ts.self.ip;
            snprintf(H.ts_name, sizeof(H.ts_name), "%s", ts.self.dns[0] ? ts.self.dns : ts.self.host);
            len = strlen(H.ts_name);
            if (len && H.ts_name[len - 1] == '.') H.ts_name[len - 1] = 0;
        }
        LeaveCriticalSection(&lk);
        logmsg("host: tailscale: %s", state == TS_RUNNING ? "running" : state == TS_ABSENT ? "not installed" : "not connected");
    }
    memset(&rcfg, 0, sizeof(rcfg));
    if (!stopping(gen) && lhost_cfg.upnp) {
        mapped = lhw_router_open(&router, &rcfg, port);
        EnterCriticalSection(&lk);
        if (gen == lobby_gen) {
            H.router = router;
            H.router_state = mapped ? RT_MAPPED : RT_FAILED;
        }
        LeaveCriticalSection(&lk);
        {
            char ext[16];
            ip_text(router.ext_ip, ext, sizeof(ext));
            logmsg("host: router: %s (internet address %s, %s)", router.said, router.ext_ip ? ext : "unknown",
                   router.wan == LH_WAN_PUBLIC ? "public" : router.wan == LH_WAN_CGNAT ? "shared by the provider (CGNAT)" :
                   router.wan == LH_WAN_PRIVATE ? "private: another router in front" : "not known");
        }
    }
    /* keep the mapping renewed while the lobby (or, after a handover, the game) runs */
    while (mapped && !stopping(gen)) {
        Sleep(500);
        if (lhw_router_renew(&router, &rcfg, GetTickCount()))
            logmsg("host: router mapping renewed");
    }
    if (mapped && !(H.keep && gen == lobby_gen)) {
        lhw_router_close(&router, &rcfg);
        logmsg("host: router mapping of UDP %u removed", port);
    }
    if (mapped && H.keep && gen == lobby_gen) {   /* the game took over the port: renewed while it runs */
        lhw_router_keep(&router, &rcfg, &H.game_end, 250);     /* until the game quits (lhost_game_end) */
        logmsg("host: router mapping of UDP %u removed: the game quits", port);
    }
    EnterCriticalSection(&lk);
    if (gen == lobby_gen && H.router_state == RT_MAPPED) H.router_state = RT_OFF;
    LeaveCriticalSection(&lk);
    WSACleanup();
    return 0;
}

void lhost_open(unsigned port, int steam)
{
    lock_make();
    lhost_close(0);
    EnterCriticalSection(&lk);
    memset(&H, 0, sizeof(H));
    H.open = 1;
    H.steam = steam;
    H.port = port;
    H.secret = random_u32();
    H.ts_state = TS_CHECKING;
    H.router_state = lhost_cfg.upnp ? RT_TRYING : RT_OFF;
    LeaveCriticalSection(&lk);
    H.thread = CreateThread(NULL, 0, worker, (void *)(INT_PTR)InterlockedIncrement(&lobby_gen), 0, NULL);
    logmsg("host: lobby %s, checking the ways in", lhost_cfg.invite_only ? "invite only" : "open to my networks");
}

void lhost_close(int keep_mapping)
{
    lock_make();
    if (!H.thread) {
        H.open = 0;
        return;
    }
    H.keep = keep_mapping;
    H.handed = keep_mapping;
    InterlockedExchange(&H.stop, 1);
    if (!keep_mapping && WaitForSingleObject(H.thread, 8000) != WAIT_OBJECT_0) {
        InterlockedIncrement(&lobby_gen);   /* it finishes (and removes any mapping) on its own, publishing nothing */
        logmsg("host: the checks did not finish in time");
    }
    if (keep_mapping)
        H.kept = H.thread;                  /* lhost_game_end waits for it to remove the mapping */
    else
        CloseHandle(H.thread);
    H.thread = NULL;
    H.open = 0;
}

/* ---- the game after the launcher (net.c, input.c) ---- */
int lhost_game_secret(uint32_t *secret)
{
    *secret = H.secret;
    return H.handed && lhost_cfg.invite_only && H.secret;
}

void lhost_set_join_key(uint32_t key)
{
    join_key = key;
    join_has_key = 1;
}

int lhost_join_key(uint32_t *key)
{
    *key = join_key;
    return join_has_key;
}

void lhost_game_end(void)
{
    if (!H.kept)
        return;
    InterlockedExchange(&H.game_end, 1);
    if (WaitForSingleObject(H.kept, 3000) != WAIT_OBJECT_0)
        logmsg("host: the router mapping was not removed in time (it lapses with its lease)");
    CloseHandle(H.kept);
    H.kept = NULL;
}

/* ---- the panel ---- */
static const char *product_fix_firewall =
    "Setup can add the rule (tick \"Allow hosting through Windows Firewall\"), or allow MDK2 when Windows asks, on Private and Public networks.";

int lhost_rows(struct lhost_row *r)
{
    char lan[16], ext[16], ts[16];
    lock_make();
    memset(r, 0, sizeof(*r) * LHOST_ROWS);
    EnterCriticalSection(&lk);
    snprintf(r[LHOST_STEAM].title, sizeof(r[0].title), "Steam");
    snprintf(r[LHOST_LAN].title, sizeof(r[0].title), "Same network");
    snprintf(r[LHOST_TAILSCALE].title, sizeof(r[0].title), "Tailscale");
    snprintf(r[LHOST_INTERNET].title, sizeof(r[0].title), "Internet");
    snprintf(r[LHOST_FIREWALL].title, sizeof(r[0].title), "Windows Firewall");
    ip_text(H.nlan ? H.lan[0].ip : 0, lan, sizeof(lan));
    ip_text(H.router.ext_ip, ext, sizeof(ext));
    ip_text(H.ts_ip, ts, sizeof(ts));

    r[LHOST_STEAM].state = LHOST_OFF;
    snprintf(r[LHOST_STEAM].result, sizeof(r[0].result), "Not used by this lobby.");
    snprintf(r[LHOST_STEAM].fix, sizeof(r[0].fix), "To host over Steam, choose Connection: Steam on Play. Friends then use Join game in Steam.");

    if (!H.nlan) {
        r[LHOST_LAN].state = H.lan_probe ? LHOST_WARN : LHOST_CHECKING;
        snprintf(r[LHOST_LAN].result, sizeof(r[0].result), H.lan_probe ? "No local network found." : "Checking...");
        if (H.lan_probe) snprintf(r[LHOST_LAN].fix, sizeof(r[0].fix), "Connect this PC to a network (cable or Wi-Fi).");
    } else if (!H.lan_probe) {
        r[LHOST_LAN].state = LHOST_CHECKING;
        snprintf(r[LHOST_LAN].result, sizeof(r[0].result), "Checking %s port %u...", lan, H.port);
    } else if (H.lan_probe == 1) {
        r[LHOST_LAN].state = LHOST_OK;
        snprintf(r[LHOST_LAN].result, sizeof(r[0].result), "This PC: %s, port %u.%s", lan, H.port,
                 lhost_cfg.invite_only ? " Invite only: friends need the invite." : " Friends see it in Join.");
    } else {
        r[LHOST_LAN].state = LHOST_WARN;
        snprintf(r[LHOST_LAN].result, sizeof(r[0].result), "The lobby did not answer on %s port %u.", lan, H.port);
        snprintf(r[LHOST_LAN].fix, sizeof(r[0].fix), "Another program may be using UDP %u: pick another port in Settings > Network.", H.port);
    }

    if (H.ts_state == TS_CHECKING) {
        r[LHOST_TAILSCALE].state = LHOST_CHECKING;
        snprintf(r[LHOST_TAILSCALE].result, sizeof(r[0].result), "Checking...");
    } else if (H.ts_state == TS_RUNNING) {
        r[LHOST_TAILSCALE].state = LHOST_OK;
        snprintf(r[LHOST_TAILSCALE].result, sizeof(r[0].result), "This PC: %s (%s).", H.ts_name[0] ? H.ts_name : ts, ts);
    } else if (H.ts_state == TS_ABSENT) {
        r[LHOST_TAILSCALE].state = LHOST_OFF;
        snprintf(r[LHOST_TAILSCALE].result, sizeof(r[0].result), "Not installed. It is optional.");
        snprintf(r[LHOST_TAILSCALE].fix, sizeof(r[0].fix), "With Tailscale (free), friends on your tailnet join without router setup.");
    } else {
        r[LHOST_TAILSCALE].state = LHOST_WARN;
        snprintf(r[LHOST_TAILSCALE].result, sizeof(r[0].result), "Installed, not connected.");
        snprintf(r[LHOST_TAILSCALE].fix, sizeof(r[0].fix), "Start Tailscale and log in, then reopen the lobby.");
    }

    if (H.router_state == RT_OFF && !lhost_cfg.upnp) {
        r[LHOST_INTERNET].state = LHOST_OFF;
        snprintf(r[LHOST_INTERNET].result, sizeof(r[0].result), "Router port: off (Settings > Network).");
        snprintf(r[LHOST_INTERNET].fix, sizeof(r[0].fix), "Forward UDP %u to %s in the router yourself, or use Steam or Tailscale.", H.port, lan);
    } else if (H.router_state == RT_TRYING) {
        r[LHOST_INTERNET].state = LHOST_CHECKING;
        snprintf(r[LHOST_INTERNET].result, sizeof(r[0].result), "Asking the router to open UDP %u...", H.port);
    } else if (H.router_state == RT_MAPPED && H.router.wan == LH_WAN_PUBLIC) {
        r[LHOST_INTERNET].state = lhost_cfg.internet_in_invite ? LHOST_OK : LHOST_WARN;
        snprintf(r[LHOST_INTERNET].result, sizeof(r[0].result), "%s Internet address %s%s", H.router.said, ext,
                 lhost_cfg.internet_in_invite ? ", in the invite." : ", left out of invites (Settings > Network).");
    } else if (H.router_state == RT_MAPPED && (H.router.wan == LH_WAN_CGNAT || H.router.wan == LH_WAN_PRIVATE)) {
        r[LHOST_INTERNET].state = LHOST_WARN;
        snprintf(r[LHOST_INTERNET].result, sizeof(r[0].result), "%s But the router's internet address is %s.", H.router.said, ext);
        snprintf(r[LHOST_INTERNET].fix, sizeof(r[0].fix), H.router.wan == LH_WAN_CGNAT ?
                 "Your internet provider shares your address with other customers, so friends cannot reach you from the internet. Use Steam or Tailscale." :
                 "Another router sits in front of this one, so friends cannot reach you from the internet. Use Steam or Tailscale.");
    } else if (H.router_state == RT_MAPPED) {
        r[LHOST_INTERNET].state = LHOST_WARN;
        snprintf(r[LHOST_INTERNET].result, sizeof(r[0].result), "%s It did not say the internet address.", H.router.said);
        snprintf(r[LHOST_INTERNET].fix, sizeof(r[0].fix), "Give friends your internet address yourself, or use Steam or Tailscale.");
    } else {
        r[LHOST_INTERNET].state = LHOST_WARN;
        snprintf(r[LHOST_INTERNET].result, sizeof(r[0].result), "%s", H.router.said[0] ? H.router.said : "The router did not open the port.");
        snprintf(r[LHOST_INTERNET].fix, sizeof(r[0].fix), "Turn on UPnP or NAT-PMP in the router, or forward UDP %u to %s. Or use Steam or Tailscale.", H.port, lan);
    }

    if (!H.fw_done) {
        r[LHOST_FIREWALL].state = LHOST_CHECKING;
        snprintf(r[LHOST_FIREWALL].result, sizeof(r[0].result), "Checking...");
    } else if (!H.fw.ok) {
        r[LHOST_FIREWALL].state = LHOST_WARN;
        snprintf(r[LHOST_FIREWALL].result, sizeof(r[0].result), "%s", H.fw.error);
        snprintf(r[LHOST_FIREWALL].fix, sizeof(r[0].fix), "%s", product_fix_firewall);
    } else {
        r[LHOST_FIREWALL].state = lh_fw_line(H.fw.private_verdict, H.fw.public_verdict, r[LHOST_FIREWALL].result,
                                             sizeof(r[0].result)) ? LHOST_OK : LHOST_WARN;
        if (r[LHOST_FIREWALL].state != LHOST_OK)
            snprintf(r[LHOST_FIREWALL].fix, sizeof(r[0].fix), "%s", product_fix_firewall);
    }
    LeaveCriticalSection(&lk);
    return LHOST_ROWS;
}

int lhost_invite(const char *name, char *message, size_t cap, char *code, size_t code_cap)
{
    struct ci_invite v;
    char c[CI_CODE_MAX];
    int internet_ok;
    lock_make();
    memset(&v, 0, sizeof(v));
    EnterCriticalSection(&lk);
    internet_ok = lhost_cfg.internet_in_invite && H.router_state == RT_MAPPED && H.router.wan == LH_WAN_PUBLIC;
    lh_invite_routes(&v, H.lan, H.nlan, H.ts_state == TS_RUNNING ? H.ts_ip : 0, H.router.ext_ip, internet_ok);
    v.flags = (uint8_t)(lhost_cfg.invite_only ? CI_FLAG_INVITE_ONLY : 0);
    v.secret = H.secret;
    v.port = (uint16_t)H.port;
    LeaveCriticalSection(&lk);
    if (!H.open || !v.n || !ci_encode(&v, "MDK2", c, sizeof(c))) return 0;
    if (code) snprintf(code, code_cap, "%s", c);
    return ci_message(name && name[0] ? name : "Player", "MDK2", LHOST_JOIN_PAGE, c, message, cap);
}

int lhost_copy_invite(HWND wnd, const char *name)
{
    char text[CI_CODE_MAX + 128];
    int n = lhost_invite(name, text, sizeof(text), NULL, 0), ok = 0;
    HGLOBAL mem;
    if (!n || !OpenClipboard(wnd)) return 0;
    EmptyClipboard();
    if ((mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n + 1)) != NULL) {
        memcpy(GlobalLock(mem), text, (size_t)n + 1);
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_TEXT, mem) != NULL;
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    logmsg("host: invite %s the clipboard", ok ? "copied to" : "could not be put on");
    return ok;
}

void lhost_dump(FILE *f)
{
    struct lhost_row r[LHOST_ROWS];
    char msg[CI_CODE_MAX + 128], code[CI_CODE_MAX];
    int i;
    lhost_rows(r);
    fprintf(f, "[host]\nopen=%d\ninvite_only=%d\nupnp=%d\nrouter_state=%d\nrouter_method=%d\nrouter_wan=%d\n"
               "router_said=%s\nfirewall_private=%d\nfirewall_public=%d\nlan_probe=%d\ntailscale=%d\nbans=%d\n",
            H.open, lhost_cfg.invite_only, lhost_cfg.upnp, H.router_state, H.router.method, H.router.wan, H.router.said,
            H.fw_done ? H.fw.private_verdict : -1, H.fw_done ? H.fw.public_verdict : -1, H.lan_probe, H.ts_state, H.bans.n);
    for (i = 0; i < LHOST_ROWS; i++)
        fprintf(f, "row%d=%d %s: %s\n", i, r[i].state, r[i].title, r[i].result);
    if (lhost_invite("Host", msg, sizeof(msg), code, sizeof(code)))
        fprintf(f, "invite_code=%s\ninvite=%s\n", code, msg);
}

/* ---- the lobby socket ---- */
int lhost_admit(const char *hello, uint32_t ip, unsigned port, int members, int limit)
{
    struct lh_hello h;
    if (!lh_hello_parse(hello, &h)) return 0;
    return lh_admit(&H.bans, lhost_cfg.invite_only, H.secret, &h, ip, port, members, limit);
}

void lhost_kick(unsigned id, uint32_t ip, unsigned port, const char *name)
{
    lh_ban(&H.bans, id, ip, port, name);
}

const char *lhost_refusal_text(int reason) { return lh_refusal_text(reason); }
int lhost_refusal_parse(const char *s, int n) { return n > 0 ? lh_refusal_parse(s, (size_t)n) : 0; }

/* ---- client ---- */
int lhost_join_targets(const char *text, unsigned default_port, struct sockaddr_in *to, uint32_t *key, int *has_key)
{
    struct cr_result r;
    int i, n = 0;
    *has_key = 0;
    *key = 0;
    coop_recognise(text, strlen(text), "MDK2", &r);
    if (r.kind == CR_INVITE || r.kind == CR_LINK) {
        for (i = 0; i < r.invite.n && n < LHOST_TARGETS; i++, n++)
            lhw_sin(&to[n], lh_ip4(r.invite.route[i].ip), r.invite.port);
        *key = r.invite.secret;
        *has_key = 1;
    } else if (r.kind == CR_ADDRESS) {
        lhw_sin(&to[n++], lh_ip4(r.ip), r.port ? r.port : default_port);
    }
    return n;
}

int lhost_hello(char *out, size_t cap, unsigned id, int ready, int rtt, unsigned long ts, const char *version,
                int has_key, uint32_t key, const char *name)
{
    return lh_hello_write(out, cap, id, ready, rtt, ts, version, has_key, key, name);
}

/* ---- pad ---- */
static struct lh_pad pad;

int lhost_pad_step(unsigned buttons, int lx, int ly, DWORD now, int *actions, int cap)
{
    return lh_pad_step(&pad, buttons, lx, ly, (uint32_t)now, actions, cap);
}

int lhost_pad_poll(DWORD now, int *actions, int cap)
{
    struct pad_raw p;
    int i;
    for (i = 0; i < 4; i++)
        if (pad_read(i, &p))
            return lhost_pad_step(p.buttons, p.lx, p.ly, now, actions, cap);
    pad.held = 0;
    return 0;
}
