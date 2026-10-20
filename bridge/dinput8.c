/*
 * mdk2-coop bridge: dinput8.dll proxy loaded by mdk2hd.exe.
 *
 * Forwards DirectInput to the system DLL and exposes co-op functions to the
 * game's Lua 3.1. Addresses are for Steam build 3902 (mdk2hd.exe sha256
 * bf5faa81...7f43); see games/mdk2-hd/NOTES.md in coop-anything for how
 * each was found. The bridge checks the build before touching anything.
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ---- game Lua 3.1 API (cdecl, statically linked in mdk2hd.exe) ---- */

typedef void (*lua_CFunction)(void);

static void (*const lua_pushnumber)(double) = (void *)0x476bb0;
/* luaL_check_lstr(narg, &len): string argument, raises a Lua error if missing */
static const char *(*const luaL_check_lstr)(int, long *) = (void *)0x478100;
/* tolua_getusertype(narg, def): C object behind a Lua argument */
static void *(*const tolua_getusertype)(int, void *) = (void *)0x475700;
/* tolua_function(module, name, fn): module NULL registers a global */
static void (*const tolua_function)(const char *, const char *, lua_CFunction) = (void *)0x4776b0;

/* om anim state for (gob, anim id): 40-byte block, NULL if the model lacks it */
static DWORD *(*const om_anim_state)(void *, int) = (void *)0x44c640;
#define ANIM_ID_MAX     160     /* exe names 146 ANIM_* constants */

/* OMGOB layout, from the tolua position/orientation accessors at 0x437af0/0x437ba0 */
#define GOB_POSITION    0x18    /* float x, y, z */
#define GOB_ORIENTATION 0x24    /* float quaternion[4]; engine setter copies it raw */
#define GOB_ANIM        0x60    /* anim controller: +0x24 state array, +0x28/+0x2c current/previous state */

/* Lua builtin table {name, fn} entry for "copytagmethods", unused by the
 * game's scripts. Repointed to coop_init before Lua starts. */
#define BUILTIN_ENTRY      ((void **)0x506520)
#define BUILTIN_ENTRY_NAME ((const char *)0x4d7d34)

/* ---- log ---- */

static FILE *log_file;
static char coop_dir[MAX_PATH];

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    if (!log_file)
        return;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    fflush(log_file);
}

/* ---- network: UDP to one peer, latest packet wins ---- */

#define PACKET_MAGIC 0x334b444d     /* "MDK3": v3 adds sender time */

#define ANIM_STATE_DWORDS 10   /* flags, mode, times, rate, weight: plain data, no pointers */
#define ANIM_TIME0        3    /* [3] and [4]: normalised play position, 0..1 */
#define ANIM_TIME1        4
#define ANIM_WEIGHT       6
#define PACKET_ANIMS      8
#define NO_ANIM           0xff

struct anim_entry {
    unsigned char id;
    DWORD state[ANIM_STATE_DWORDS];
};

struct packet {
    unsigned int magic;
    unsigned int seq;
    unsigned int time;          /* sender clock, ms */
    float pos[3];
    float quat[4];              /* w, x, y, z */
    unsigned char cur, prev, nanims;
    struct anim_entry anims[PACKET_ANIMS];
};

/* settings from coop.ini */
static int sync_anim = 1;       /* [sync] anim: 0 turns animation sync off */
static int send_hz = 30;        /* [sync] send_hz */
static int interp_ms = 100;     /* [sync] interp_ms: render the peer this far in the past; 0 = latest packet */
static int extrap_ms = 150;     /* [sync] extrap_ms: max extrapolation when packets are late */
static int sim_latency_ms;      /* [sim] latency_ms, jitter_ms, loss_pct: fake a bad network */
static int sim_jitter_ms;
static int sim_loss_pct;

static SOCKET sock = INVALID_SOCKET;
static struct sockaddr_in peer;
static unsigned int send_seq, recv_seq;
static int have_remote;

static double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void net_open(void)
{
    char ini[MAX_PATH], host[64];
    int listen_port, peer_port;
    struct sockaddr_in local;
    u_long nonblock = 1;
    WSADATA wsa;

    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    listen_port = GetPrivateProfileIntA("net", "listen_port", 27900, ini);
    peer_port = GetPrivateProfileIntA("net", "peer_port", 27901, ini);
    GetPrivateProfileStringA("net", "peer_host", "127.0.0.1", host, sizeof(host), ini);
    sync_anim = GetPrivateProfileIntA("sync", "anim", 1, ini);
    send_hz = GetPrivateProfileIntA("sync", "send_hz", 30, ini);
    interp_ms = GetPrivateProfileIntA("sync", "interp_ms", 100, ini);
    extrap_ms = GetPrivateProfileIntA("sync", "extrap_ms", 150, ini);
    sim_latency_ms = GetPrivateProfileIntA("sim", "latency_ms", 0, ini);
    sim_jitter_ms = GetPrivateProfileIntA("sim", "jitter_ms", 0, ini);
    sim_loss_pct = GetPrivateProfileIntA("sim", "loss_pct", 0, ini);
    if (send_hz < 1)
        send_hz = 1;
    logmsg("sync: send %d Hz, interp %d ms, extrap %d ms, anim %d; sim: latency %d ms, jitter %d ms, loss %d%%",
           send_hz, interp_ms, extrap_ms, sync_anim, sim_latency_ms, sim_jitter_ms, sim_loss_pct);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        logmsg("WSAStartup failed");
        return;
    }
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons((u_short)listen_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock == INVALID_SOCKET || bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        logmsg("udp bind %d failed: %d", listen_port, WSAGetLastError());
        closesocket(sock);
        sock = INVALID_SOCKET;
        return;
    }
    ioctlsocket(sock, FIONBIO, &nonblock);

    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons((u_short)peer_port);
    peer.sin_addr.s_addr = inet_addr(host);
    logmsg("udp listen %d, peer %s:%d", listen_port, host, peer_port);
}

/* ---- snapshot buffer ---- */

#define SNAPS 32

static struct packet snaps[SNAPS];     /* ordered by sender time, oldest first */
static int nsnaps;
static double clock_offset;            /* sender clock - local clock, least-delayed estimate */

static void snap_add(const struct packet *p, double arrived)
{
    double o = (double)p->time - arrived;
    int i;

    if (!have_remote || o > clock_offset)
        clock_offset = o;
    else
        clock_offset -= 0.01;          /* let the estimate follow slow clock drift */

    if (nsnaps == SNAPS) {
        memmove(snaps, snaps + 1, sizeof(snaps[0]) * (SNAPS - 1));
        nsnaps--;
    }
    for (i = nsnaps; i > 0 && (int)(snaps[i - 1].time - p->time) > 0; i--)
        snaps[i] = snaps[i - 1];
    snaps[i] = *p;
    nsnaps++;
}

/* ---- simulated network: hold packets back, drop some ---- */

#define SIM_QUEUE 256

static struct { struct packet p; double due; } simq[SIM_QUEUE];
static int nsimq;

static void accept_packet(const struct packet *p, double now)
{
    if (have_remote && p->seq <= recv_seq)
        return;                         /* late or duplicate */
    snap_add(p, now);
    recv_seq = p->seq;
    if (p->seq % 300 == 0)
        logmsg("recv #%u pos (%.2f, %.2f, %.2f) quat (%.3f, %.3f, %.3f, %.3f) anim cur %d n %d", p->seq,
               p->pos[0], p->pos[1], p->pos[2], p->quat[0], p->quat[1], p->quat[2], p->quat[3], p->cur, p->nanims);
    if (!have_remote)
        logmsg("first packet from peer (seq %u)", p->seq);
    have_remote = 1;
}

static void net_poll(void)
{
    struct packet p;
    double now = now_ms();
    int n, i;

    if (sock == INVALID_SOCKET)
        return;
    while ((n = recv(sock, (char *)&p, sizeof(p), 0)) == sizeof(p)) {
        if (p.magic != PACKET_MAGIC || p.nanims > PACKET_ANIMS)
            continue;
        if (!sim_latency_ms && !sim_jitter_ms && !sim_loss_pct) {
            accept_packet(&p, now);
            continue;
        }
        if (rand() % 100 < sim_loss_pct || nsimq == SIM_QUEUE)
            continue;
        simq[nsimq].p = p;
        simq[nsimq].due = now + sim_latency_ms + (sim_jitter_ms ? rand() % (sim_jitter_ms + 1) : 0);
        nsimq++;
    }
    for (i = 0; i < nsimq; ) {
        if (simq[i].due <= now) {
            accept_packet(&simq[i].p, simq[i].due);
            simq[i] = simq[--nsimq];
        } else {
            i++;
        }
    }
}

/* ---- interpolation ---- */

static struct packet view;      /* what the puppet shows this frame */

static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* lerp a 0..1 play position the short way round */
static float lerp_phase(float a, float b, float t)
{
    float v;

    if (b - a > 0.5f)
        a += 1.0f;
    else if (a - b > 0.5f)
        b += 1.0f;
    v = lerpf(a, b, t);
    return v >= 1.0f ? v - 1.0f : v;
}

static void nlerp_quat(float *out, const float *a, const float *b, float t)
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float sign = dot < 0.0f ? -1.0f : 1.0f, len = 0.0f;
    int i;

    for (i = 0; i < 4; i++) {
        out[i] = lerpf(a[i], sign * b[i], t);
        len += out[i] * out[i];
    }
    len = len > 0.0f ? 1.0f / sqrtf(len) : 1.0f;
    for (i = 0; i < 4; i++)
        out[i] *= len;
}

/* Blend two snapshots: position and rotation lerped, anims from the nearer one with
 * play positions and weights blended where both have the same anim */
static void blend(struct packet *out, const struct packet *a, const struct packet *b, float t)
{
    const struct packet *other = t < 0.5f ? b : a;
    int i, j, k;

    *out = t < 0.5f ? *a : *b;
    for (k = 0; k < 3; k++)
        out->pos[k] = lerpf(a->pos[k], b->pos[k], t);
    nlerp_quat(out->quat, a->quat, b->quat, t);
    for (i = 0; i < out->nanims; i++) {
        for (j = 0; j < other->nanims && other->anims[j].id != out->anims[i].id; j++)
            ;
        if (j == other->nanims)
            continue;
        {
            const DWORD *sa = t < 0.5f ? out->anims[i].state : other->anims[j].state;
            const DWORD *sb = t < 0.5f ? other->anims[j].state : out->anims[i].state;
            float *d = (float *)out->anims[i].state;
            d[ANIM_TIME0] = lerp_phase(((const float *)sa)[ANIM_TIME0], ((const float *)sb)[ANIM_TIME0], t);
            d[ANIM_TIME1] = lerp_phase(((const float *)sa)[ANIM_TIME1], ((const float *)sb)[ANIM_TIME1], t);
            d[ANIM_WEIGHT] = lerpf(((const float *)sa)[ANIM_WEIGHT], ((const float *)sb)[ANIM_WEIGHT], t);
        }
    }
}

static void update_view(void)
{
    double render;
    int i;

    if (!nsnaps)
        return;
    if (interp_ms <= 0 || nsnaps == 1) {
        view = snaps[nsnaps - 1];
        return;
    }
    render = now_ms() + clock_offset - interp_ms;
    for (i = nsnaps - 1; i > 0 && (double)snaps[i - 1].time > render; i--)
        ;
    /* snaps[i-1] .. snaps[i] bracket render, or render is outside the buffer */
    if (i == 0) {
        view = snaps[0];
    } else if ((double)snaps[i].time >= render) {
        const struct packet *a = &snaps[i - 1], *b = &snaps[i];
        float span = (float)(b->time - a->time);
        blend(&view, a, b, span > 0.0f ? (float)(render - a->time) / span : 1.0f);
    } else {
        /* newest packet is older than render time: extrapolate along the last step */
        const struct packet *a = &snaps[nsnaps - 2], *b = &snaps[nsnaps - 1];
        float span = (float)(b->time - a->time);
        double ahead = render - b->time;
        int k;

        if (ahead > extrap_ms)
            ahead = extrap_ms;
        view = *b;
        if (span > 0.0f)
            for (k = 0; k < 3; k++)
                view.pos[k] = b->pos[k] + (b->pos[k] - a->pos[k]) * (float)(ahead / span);
    }
}

/* ---- smoothness stats: how evenly the puppet moves from frame to frame ---- */

static struct {
    float last[3];
    int have_last, frames, stalls;
    double sum, sumsq, max, window_start;
} smooth;

static void smooth_track(const float *pos)
{
    double now = now_ms(), step = 0.0;
    int k;

    if (smooth.have_last) {
        for (k = 0; k < 3; k++)
            step += (double)(pos[k] - smooth.last[k]) * (pos[k] - smooth.last[k]);
        step = sqrt(step);
        smooth.frames++;
        smooth.sum += step;
        smooth.sumsq += step * step;
        if (step > smooth.max)
            smooth.max = step;
        if (step < 1e-4)
            smooth.stalls++;
    } else {
        smooth.window_start = now;
    }
    memcpy(smooth.last, pos, sizeof(smooth.last));
    smooth.have_last = 1;

    if (now - smooth.window_start >= 2000.0 && smooth.frames) {
        double mean = smooth.sum / smooth.frames;
        double sd = sqrt(fmax(0.0, smooth.sumsq / smooth.frames - mean * mean));
        if (mean > 0.005)       /* only report windows where the peer is moving */
            logmsg("smooth: %d frames, step mean %.4f sd %.4f max %.4f, stalled %d%%, cv %.2f",
                   smooth.frames, mean, sd, smooth.max, smooth.stalls * 100 / smooth.frames, sd / mean);
        memset(&smooth, 0, sizeof(smooth));
    }
}

/* ---- Lua functions ---- */

/* Anim id whose state block is at st, or NO_ANIM */
static unsigned char anim_id_of(BYTE *gob, DWORD *st)
{
    int id;

    if (!st)
        return NO_ANIM;
    for (id = 0; id < ANIM_ID_MAX; id++)
        if (om_anim_state(gob, id) == st)
            return (unsigned char)id;
    return NO_ANIM;
}

static void anim_capture(BYTE *gob, struct packet *p)
{
    BYTE *ctl = *(BYTE **)(gob + GOB_ANIM);
    int id;

    p->cur = p->prev = NO_ANIM;
    p->nanims = 0;
    if (!ctl)
        return;
    p->cur = anim_id_of(gob, *(DWORD **)(ctl + 0x28));
    p->prev = anim_id_of(gob, *(DWORD **)(ctl + 0x2c));
    for (id = 0; id < ANIM_ID_MAX && p->nanims < PACKET_ANIMS; id++) {
        DWORD *st = om_anim_state(gob, id);
        if (st && st[0]) {
            p->anims[p->nanims].id = (unsigned char)id;
            memcpy(p->anims[p->nanims].state, st, sizeof(p->anims[0].state));
            p->nanims++;
        }
    }
}

/* Make gob's anim states match the packet: copy active blocks, stop the rest */
static void anim_apply(BYTE *gob, const struct packet *p)
{
    BYTE *ctl = *(BYTE **)(gob + GOB_ANIM);
    int id, i;

    if (!ctl)
        return;
    for (id = 0; id < ANIM_ID_MAX; id++) {
        DWORD *st = om_anim_state(gob, id);
        if (!st)
            continue;
        for (i = 0; i < p->nanims && p->anims[i].id != id; i++)
            ;
        if (i < p->nanims)
            memcpy(st, p->anims[i].state, sizeof(p->anims[0].state));
        else
            st[0] = 0;
    }
    *(DWORD **)(ctl + 0x28) = p->cur != NO_ANIM ? om_anim_state(gob, p->cur) : NULL;
    *(DWORD **)(ctl + 0x2c) = p->prev != NO_ANIM ? om_anim_state(gob, p->prev) : NULL;
}

/* coop_send(gob): send the gob's position, orientation and animation, at most send_hz times a second */
static void l_coop_send(void)
{
    static double next_send;
    struct packet p;
    BYTE *gob = tolua_getusertype(1, NULL);
    double now = now_ms();

    if (sock == INVALID_SOCKET || !gob || now < next_send)
        return;
    next_send = (now - next_send > 1000.0 / send_hz) ? now + 1000.0 / send_hz : next_send + 1000.0 / send_hz;
    p.magic = PACKET_MAGIC;
    p.seq = ++send_seq;
    p.time = (unsigned int)now;
    memcpy(p.pos, gob + GOB_POSITION, sizeof(p.pos));
    memcpy(p.quat, gob + GOB_ORIENTATION, sizeof(p.quat));
    anim_capture(gob, &p);
    sendto(sock, (const char *)&p, sizeof(p), 0, (struct sockaddr *)&peer, sizeof(peer));
    if (p.seq % 300 == 0)
        logmsg("send #%u pos (%.2f, %.2f, %.2f) quat (%.3f, %.3f, %.3f, %.3f) anim cur %d n %d", p.seq,
               p.pos[0], p.pos[1], p.pos[2], p.quat[0], p.quat[1], p.quat[2], p.quat[3], p.cur, p.nanims);
}

/* coop_recv() -> x, y, z of the peer for this frame, or nil before the first packet */
static void l_coop_recv(void)
{
    net_poll();
    if (!have_remote)
        return;
    update_view();
    smooth_track(view.pos);
    lua_pushnumber(view.pos[0]);
    lua_pushnumber(view.pos[1]);
    lua_pushnumber(view.pos[2]);
}

/* coop_face(gob): give the gob the peer's orientation and animation for this frame */
static void l_coop_face(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);

    if (!gob || !have_remote)
        return;
    memcpy(gob + GOB_ORIENTATION, view.quat, sizeof(view.quat));
    if (sync_anim)
        anim_apply(gob, &view);
}

/* coop_animdump(gob): log active anim states, for reverse engineering */
static void l_coop_animdump(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);
    BYTE *ctl;
    int id, i;

    if (!gob || !(ctl = *(BYTE **)(gob + GOB_ANIM)))
        return;
    logmsg("animdump gob=%p pos (%.2f, %.2f, %.2f) ctl=%p states=%p cur=%p prev=%p", (void *)gob,
           ((float *)(gob + GOB_POSITION))[0], ((float *)(gob + GOB_POSITION))[1],
           ((float *)(gob + GOB_POSITION))[2], (void *)ctl,
           *(void **)(ctl + 0x24), *(void **)(ctl + 0x28), *(void **)(ctl + 0x2c));
    for (id = 0; id < ANIM_ID_MAX; id++) {
        DWORD *st = om_anim_state(gob, id);
        char line[256];
        int n;

        if (!st || !st[0])
            continue;
        n = snprintf(line, sizeof(line), "  anim %3d @%p:", id, (void *)st);
        for (i = 0; i < 10; i++)
            n += snprintf(line + n, sizeof(line) - n, " %08lx", (unsigned long)st[i]);
        logmsg("%s", line);
    }
}

/* coop_log(msg): write a line to bridge.log */
static void l_coop_log(void)
{
    long len;
    logmsg("lua: %s", luaL_check_lstr(1, &len));
}

/* coop_init(): called once from Lua; registers the rest and opens the socket */
static void l_coop_init(void)
{
    static int done;

    if (done)
        return;
    done = 1;
    tolua_function(NULL, "coop_send", l_coop_send);
    tolua_function(NULL, "coop_recv", l_coop_recv);
    tolua_function(NULL, "coop_face", l_coop_face);
    tolua_function(NULL, "coop_animdump", l_coop_animdump);
    tolua_function(NULL, "coop_log", l_coop_log);
    net_open();
    logmsg("coop_init: lua functions registered");
}

static void install_lua_hook(void)
{
    void **entry = BUILTIN_ENTRY;
    DWORD old;

    if (IsBadReadPtr(entry, 8) || IsBadReadPtr(BUILTIN_ENTRY_NAME, 15) ||
        entry[0] != (void *)BUILTIN_ENTRY_NAME || strcmp(BUILTIN_ENTRY_NAME, "copytagmethods") != 0) {
        logmsg("unknown game build: lua hook not installed");
        return;
    }
    VirtualProtect(entry, 8, PAGE_READWRITE, &old);
    entry[0] = (void *)"coop_init";
    entry[1] = (void *)l_coop_init;
    VirtualProtect(entry, 8, old, &old);
    logmsg("lua hook installed");
}

/* ---- dinput8 forwarding ---- */

static HMODULE real_dinput8;

static FARPROC real_proc(const char *name)
{
    if (!real_dinput8) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\dinput8.dll");
        real_dinput8 = LoadLibraryA(path);
        logmsg("loaded %s -> %p", path, (void *)real_dinput8);
    }
    return real_dinput8 ? GetProcAddress(real_dinput8, name) : NULL;
}

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    DirectInput8Create_t fn = (DirectInput8Create_t)real_proc("DirectInput8Create");
    return fn ? fn(inst, ver, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DllCanUnloadNow(void)
{
    FARPROC fn = real_proc("DllCanUnloadNow");
    return fn ? ((HRESULT (WINAPI *)(void))fn)() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *out)
{
    FARPROC fn = real_proc("DllGetClassObject");
    return fn ? ((HRESULT (WINAPI *)(REFCLSID, REFIID, LPVOID *))fn)(clsid, riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_DllRegisterServer(void)
{
    FARPROC fn = real_proc("DllRegisterServer");
    return fn ? ((HRESULT (WINAPI *)(void))fn)() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer(void)
{
    FARPROC fn = real_proc("DllUnregisterServer");
    return fn ? ((HRESULT (WINAPI *)(void))fn)() : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH], *slash;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(NULL, coop_dir, MAX_PATH);
        slash = strrchr(coop_dir, '\\');
        if (slash)
            *slash = 0;
        strcat(coop_dir, "\\coop");
        CreateDirectoryA(coop_dir, NULL);
        snprintf(path, MAX_PATH, "%s\\bridge.log", coop_dir);
        log_file = fopen(path, "w");
        logmsg("mdk2-coop bridge attached");
        install_lua_hook();
    }
    return TRUE;
}
