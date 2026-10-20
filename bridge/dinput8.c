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

/* ---- game Lua 3.1 API (cdecl, statically linked in mdk2hd.exe) ---- */

typedef void (*lua_CFunction)(void);

static void (*const lua_pushnumber)(double) = (void *)0x476bb0;
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

static FILE *logf;
static char coop_dir[MAX_PATH];

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    if (!logf)
        return;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);
}

/* ---- network: UDP to one peer, latest packet wins ---- */

#define PACKET_MAGIC 0x324b444d     /* "MDK2" */

#define ANIM_STATE_DWORDS 10   /* flags, mode, times, rate, weight: plain data, no pointers */
#define PACKET_ANIMS      8
#define NO_ANIM           0xff

struct anim_entry {
    unsigned char id;
    DWORD state[ANIM_STATE_DWORDS];
};

struct packet {
    unsigned int magic;
    unsigned int seq;
    float pos[3];
    float quat[4];
    unsigned char cur, prev, nanims;
    struct anim_entry anims[PACKET_ANIMS];
};

static SOCKET sock = INVALID_SOCKET;
static struct sockaddr_in peer;
static unsigned int send_seq, recv_seq;
static struct packet remote;
static int have_remote;
static int sync_anim = 1;       /* coop.ini [sync] anim=0 turns animation sync off */

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

static void net_poll(void)
{
    struct packet p;
    int n;

    if (sock == INVALID_SOCKET)
        return;
    while ((n = recv(sock, (char *)&p, sizeof(p), 0)) == sizeof(p)) {
        if (p.magic != PACKET_MAGIC || p.nanims > PACKET_ANIMS || (have_remote && p.seq <= recv_seq))
            continue;
        remote = p;
        recv_seq = p.seq;
        if (p.seq % 300 == 0)
            logmsg("recv #%u pos (%.2f, %.2f, %.2f) quat (%.3f, %.3f, %.3f, %.3f) anim cur %d n %d", p.seq,
                   p.pos[0], p.pos[1], p.pos[2], p.quat[0], p.quat[1], p.quat[2], p.quat[3], p.cur, p.nanims);
        if (!have_remote)
            logmsg("first packet from peer (seq %u)", p.seq);
        have_remote = 1;
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

/* coop_send(gob): send the gob's position, orientation and animation to the peer */
static void l_coop_send(void)
{
    struct packet p;
    BYTE *gob = tolua_getusertype(1, NULL);

    if (sock == INVALID_SOCKET || !gob)
        return;
    p.magic = PACKET_MAGIC;
    p.seq = ++send_seq;
    memcpy(p.pos, gob + GOB_POSITION, sizeof(p.pos));
    memcpy(p.quat, gob + GOB_ORIENTATION, sizeof(p.quat));
    anim_capture(gob, &p);
    sendto(sock, (const char *)&p, sizeof(p), 0, (struct sockaddr *)&peer, sizeof(peer));
    if (p.seq % 300 == 0)
        logmsg("send #%u pos (%.2f, %.2f, %.2f) quat (%.3f, %.3f, %.3f, %.3f) anim cur %d n %d", p.seq,
               p.pos[0], p.pos[1], p.pos[2], p.quat[0], p.quat[1], p.quat[2], p.quat[3], p.cur, p.nanims);
}

/* coop_recv() -> x, y, z  or nil before the first packet */
static void l_coop_recv(void)
{
    net_poll();
    if (!have_remote)
        return;
    lua_pushnumber(remote.pos[0]);
    lua_pushnumber(remote.pos[1]);
    lua_pushnumber(remote.pos[2]);
}

/* coop_face(gob): give the gob the peer's latest orientation and animation */
static void l_coop_face(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);

    if (!gob || !have_remote)
        return;
    memcpy(gob + GOB_ORIENTATION, remote.quat, sizeof(remote.quat));
    if (sync_anim)
        anim_apply(gob, &remote);
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
        logf = fopen(path, "w");
        logmsg("mdk2-coop bridge attached");
        install_lua_hook();
    }
    return TRUE;
}
