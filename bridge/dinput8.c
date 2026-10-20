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

typedef unsigned int lua_Object;
typedef void (*lua_CFunction)(void);

#define LUA_NOOBJECT 0

static lua_Object (*const lua_getparam)(int) = (void *)0x476690;
static int (*const lua_isnumber)(lua_Object) = (void *)0x476a20;
static double (*const lua_getnumber)(lua_Object) = (void *)0x476a60;
static void (*const lua_pushnumber)(double) = (void *)0x476bb0;
/* tolua_function(module, name, fn): module NULL registers a global */
static void (*const tolua_function)(const char *, const char *, lua_CFunction) = (void *)0x4776b0;

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

struct packet {
    unsigned int magic;
    unsigned int seq;
    float x, y, z, yaw;
};

static SOCKET sock = INVALID_SOCKET;
static struct sockaddr_in peer;
static unsigned int send_seq, recv_seq;
static struct packet remote;
static int have_remote;

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
        if (p.magic != PACKET_MAGIC || (have_remote && p.seq <= recv_seq))
            continue;
        remote = p;
        recv_seq = p.seq;
        if (p.seq % 300 == 0)
            logmsg("recv #%u (%.2f, %.2f, %.2f)", p.seq, p.x, p.y, p.z);
        if (!have_remote)
            logmsg("first packet from peer (seq %u)", p.seq);
        have_remote = 1;
    }
}

/* ---- Lua functions ---- */

static double num_param(int n)
{
    lua_Object o = lua_getparam(n);
    return (o != LUA_NOOBJECT && lua_isnumber(o)) ? lua_getnumber(o) : 0.0;
}

/* coop_send(x, y, z, yaw) */
static void l_coop_send(void)
{
    struct packet p;

    if (sock == INVALID_SOCKET)
        return;
    p.magic = PACKET_MAGIC;
    p.seq = ++send_seq;
    p.x = (float)num_param(1);
    p.y = (float)num_param(2);
    p.z = (float)num_param(3);
    p.yaw = (float)num_param(4);
    sendto(sock, (const char *)&p, sizeof(p), 0, (struct sockaddr *)&peer, sizeof(peer));
    if (p.seq % 300 == 0)
        logmsg("send #%u (%.2f, %.2f, %.2f)", p.seq, p.x, p.y, p.z);
}

/* coop_recv() -> x, y, z, yaw  or nil before the first packet */
static void l_coop_recv(void)
{
    net_poll();
    if (!have_remote)
        return;
    lua_pushnumber(remote.x);
    lua_pushnumber(remote.y);
    lua_pushnumber(remote.z);
    lua_pushnumber(remote.yaw);
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
