/* Windows adapter for coop_discover.h: up IPv4 adapters with their prefix lengths, the
 * `tailscale status --json` text, and a non-blocking UDP prober for one scan window.
 * Include after <winsock2.h>/<windows.h> are available (this header includes them itself; put
 * it before any plain <windows.h> include, or define WIN32_LEAN_AND_MEAN). Link ws2_32.
 * iphlpapi.dll is loaded on demand from the system folder, so no extra import is needed.
 * Nothing here writes files or keeps what it reads. MIT licence. */
#ifndef COOP_DISCOVER_WIN_H
#define COOP_DISCOVER_WIN_H
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include "coop_discover.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

struct cdw_adapter { struct cd_adapter a; unsigned long type; char name[64]; };
/* Vista+ process attribute API, looked up at run time so any _WIN32_WINNT works. */
typedef struct { STARTUPINFOA StartupInfo; void *lpAttributeList; } cdw_startupinfoex;
typedef BOOL (WINAPI *cdw_attr_init_fn)(void *, DWORD, DWORD, SIZE_T *);
typedef BOOL (WINAPI *cdw_attr_update_fn)(void *, DWORD, DWORD_PTR, void *, SIZE_T, void *, SIZE_T *);
typedef void (WINAPI *cdw_attr_delete_fn)(void *);
#define CDW_ATTRIBUTE_HANDLE_LIST 0x00020002u
#define CDW_EXTENDED_STARTUPINFO 0x00080000u

/* Every unicast IPv4 address of every adapter that is up (Ethernet, Wi-Fi, VPN/TAP, WireGuard,
 * Tailscale, Hyper-V switches), loopback excluded. Returns the count, or -1 if unavailable. */
static inline int cdw_adapters(struct cdw_adapter *out, int max)
{
    typedef ULONG (WINAPI *gaa_fn)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
    HMODULE lib = LoadLibraryExA("iphlpapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    gaa_fn gaa;
    IP_ADAPTER_ADDRESSES *list = NULL, *p;
    ULONG size = 32768, result = ERROR_BUFFER_OVERFLOW;
    int n = 0, tries;
    if (!lib) lib = LoadLibraryA("iphlpapi.dll");     /* systems without the search flag (pre-KB2533623) */
    gaa = lib ? (gaa_fn)(void (*)(void))GetProcAddress(lib, "GetAdaptersAddresses") : NULL;
    if (!gaa) { if (lib) FreeLibrary(lib); return -1; }
    for (tries = 0; tries < 4 && result == ERROR_BUFFER_OVERFLOW; tries++) {
        if (list) HeapFree(GetProcessHeap(), 0, list);
        list = (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(), 0, size);
        if (!list) break;
        result = gaa(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, list, &size);
    }
    if (result != ERROR_SUCCESS) n = -1;
    for (p = result == ERROR_SUCCESS ? list : NULL; p && n < max; p = p->Next) {
        IP_ADAPTER_UNICAST_ADDRESS_LH *u;   /* the Vista+ layout, whatever _WIN32_WINNT the caller builds for */
        if (p->OperStatus != IfOperStatusUp || p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (u = (IP_ADAPTER_UNICAST_ADDRESS_LH *)p->FirstUnicastAddress; u && n < max; u = u->Next) {
            const struct sockaddr_in *sin = (const struct sockaddr_in *)u->Address.lpSockaddr;
            if (!sin || sin->sin_family != AF_INET) continue;
            out[n].a.ip = ntohl(sin->sin_addr.s_addr);
            out[n].a.prefix = u->OnLinkPrefixLength;
            out[n].type = p->IfType;
            if (!p->FriendlyName || !WideCharToMultiByte(CP_UTF8, 0, p->FriendlyName, -1, out[n].name, (int)sizeof(out[n].name), NULL, NULL))
                strcpy(out[n].name, "adapter");
            n++;
        }
    }
    if (list) HeapFree(GetProcessHeap(), 0, list);
    FreeLibrary(lib);
    return n;
}
static inline int cdw_adapter_list(const struct cdw_adapter *w, int n, struct cd_adapter *out)
{
    int i;
    for (i = 0; i < n; i++) out[i] = w[i].a;
    return n;
}

/* tailscale.exe: each PATH folder, then %ProgramW6432% (the 64-bit Program Files a 32-bit process
 * does not see as %ProgramFiles%), then %ProgramFiles%. The current folder is never searched. */
static inline int cdw_tailscale_path(char *path, size_t cap)
{
    char env[32768];                       /* on the caller's stack: callers run on worker threads */
    static const char *const roots[] = { "ProgramW6432", "ProgramFiles" };
    char *dir, *next;
    DWORD n = GetEnvironmentVariableA("PATH", env, sizeof(env));
    size_t i;
    if (n && n < sizeof(env)) {
        for (dir = env; dir && *dir; dir = next) {
            size_t len;
            next = strchr(dir, ';');
            if (next) *next++ = 0;
            if (*dir == '"') { dir++; if ((len = strlen(dir)) && dir[len - 1] == '"') dir[len - 1] = 0; }
            len = strlen(dir);
            if (!len || dir[0] == '.' || len + 16 >= cap) continue;
            snprintf(path, cap, "%s%stailscale.exe", dir, dir[len - 1] == '\\' ? "" : "\\");
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && !(GetFileAttributesA(path) & FILE_ATTRIBUTE_DIRECTORY)) return 1;
        }
    }
    for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        n = GetEnvironmentVariableA(roots[i], env, MAX_PATH);
        if (!n || n >= MAX_PATH || strlen(env) + 26 >= cap) continue;
        snprintf(path, cap, "%s\\Tailscale\\tailscale.exe", env);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    path[0] = 0;
    return 0;
}
/* Runs `tailscale status --json` with no window, stdout into out (cap bytes, NUL-terminated).
 * Returns the byte count, 0 when Tailscale is not installed, -1 on failure or timeout (the child
 * is then ended through its own process handle). Only the pipe handle is inherited. */
static inline int cdw_tailscale_status(char *out, size_t cap, DWORD timeout_ms)
{
    char path[MAX_PATH], cmd[MAX_PATH + 32];
    SECURITY_ATTRIBUTES sa;
    cdw_startupinfoex si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL, nul, inherit[2];
    void *attrs = NULL;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    cdw_attr_init_fn attr_init = k32 ? (cdw_attr_init_fn)(void (*)(void))GetProcAddress(k32, "InitializeProcThreadAttributeList") : NULL;
    cdw_attr_update_fn attr_update = k32 ? (cdw_attr_update_fn)(void (*)(void))GetProcAddress(k32, "UpdateProcThreadAttribute") : NULL;
    cdw_attr_delete_fn attr_delete = k32 ? (cdw_attr_delete_fn)(void (*)(void))GetProcAddress(k32, "DeleteProcThreadAttributeList") : NULL;
    SIZE_T attr_size = 0;
    DWORD start, code = 1;
    size_t used = 0;
    int ok, done = 0;
    if (cap < 2) return -1;
    out[0] = 0;
    if (!cdw_tailscale_path(path, sizeof(path))) return 0;
    snprintf(cmd, sizeof(cmd), "\"%s\" status --json", path);
    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) { CloseHandle(rd); CloseHandle(wr); return -1; }
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul; si.StartupInfo.hStdOutput = wr; si.StartupInfo.hStdError = nul;
    inherit[0] = wr; inherit[1] = nul;
    ok = 0;
    if (attr_init && attr_update && attr_delete) {
        attr_init(NULL, 1, 0, &attr_size);
        attrs = attr_size ? HeapAlloc(GetProcessHeap(), 0, attr_size) : NULL;
        ok = attrs && attr_init(attrs, 1, 0, &attr_size);
        if (ok && !attr_update(attrs, 0, CDW_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), NULL, NULL)) { attr_delete(attrs); ok = 0; }
    }
    si.lpAttributeList = ok ? attrs : NULL;
    if (ok) si.StartupInfo.cb = sizeof(si);
    else si.StartupInfo.cb = sizeof(si.StartupInfo);
    ok = CreateProcessA(path, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | (ok ? CDW_EXTENDED_STARTUPINFO : 0), NULL, NULL,
                        &si.StartupInfo, &pi);
    if (attrs) { if (si.lpAttributeList) attr_delete(attrs); HeapFree(GetProcessHeap(), 0, attrs); }
    CloseHandle(wr); CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }
    start = GetTickCount();
    while (!done) {
        DWORD avail = 0, got = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) { done = 1; break; }   /* child closed its end */
        if (avail) {
            DWORD room = (DWORD)(cap - 1 - used);
            if (!room) { done = -1; break; }                                          /* larger than the caller allows */
            if (!ReadFile(rd, out + used, avail < room ? avail : room, &got, NULL)) { done = 1; break; }
            used += got;
            continue;
        }
        if (GetTickCount() - start >= timeout_ms) { done = -1; break; }
        Sleep(10);
    }
    if (done < 0 || WaitForSingleObject(pi.hProcess, done < 0 ? 0 : 500) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        done = -1;
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    out[used] = 0;
    return done < 0 ? -1 : (int)used;
}

/* One scan: send CPQ2 with this nonce to every candidate from one socket, then collect replies for
 * window_ms. Only replies matching a candidate (cd_match), the nonce and the game id reach
 * on_reply. bind_ip 0 = any local address (production); tests bind a loopback address.
 * Returns the number of queries sent, or -1 when no socket could be opened. */
typedef void (*cdw_reply_fn)(void *ctx, int candidate, uint32_t ip, unsigned port, const struct cd_reply *reply, uint32_t rtt);
static inline int cdw_scan(const struct cd_candidates *c, uint32_t nonce, const char *game_id, DWORD window_ms, uint32_t bind_ip,
                    cdw_reply_fn on_reply, void *ctx, volatile LONG *stop)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in local;
    uint8_t query[CD_QUERY_LEN];
    BOOL yes = TRUE, no = FALSE;
    u_long nonblock = 1;
    DWORD start, bytes;
    int i, sent = 0;
    if (s == INVALID_SOCKET) return -1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));
    ioctlsocket(s, FIONBIO, &nonblock);
    WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof(no), NULL, 0, &bytes, NULL, NULL);  /* ICMP unreachable must not end the scan */
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(bind_ip);
    if (bind(s, (struct sockaddr *)&local, sizeof(local))) { closesocket(s); return -1; }
    cd_query(query, nonce, game_id, 0);
    start = GetTickCount();
    for (i = 0; i < c->n; i++) {
        struct sockaddr_in to;
        int tries;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(c->c[i].ip); to.sin_port = htons(c->c[i].port);
        for (tries = 0; tries < 3; tries++) {
            if (sendto(s, (const char *)query, sizeof(query), 0, (struct sockaddr *)&to, sizeof(to)) == (int)sizeof(query)) { sent++; break; }
            if (WSAGetLastError() != WSAEWOULDBLOCK) break;
            Sleep(1);
        }
    }
    while (!(stop && InterlockedCompareExchange(stop, 0, 0))) {
        DWORD elapsed = GetTickCount() - start;
        fd_set ready;
        struct timeval wait;
        if (elapsed >= window_ms) break;
        FD_ZERO(&ready); FD_SET(s, &ready);
        wait.tv_sec = 0; wait.tv_usec = (long)((window_ms - elapsed > 50 ? 50 : window_ms - elapsed) * 1000);
        if (select(0, &ready, NULL, NULL, &wait) <= 0) continue;
        for (;;) {
            uint8_t reply[CD_REPLY_MAX + 1];
            struct sockaddr_in from;
            struct cd_reply parsed;
            int len = sizeof(from), n = recvfrom(s, (char *)reply, sizeof(reply), 0, (struct sockaddr *)&from, &len), at;
            if (n < 0) { if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE) continue; break; }
            at = cd_match(c, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));
            if (at >= 0 && cd_parse_reply(reply, (size_t)n, nonce, game_id, &parsed) && on_reply)
                on_reply(ctx, at, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), &parsed, GetTickCount() - start);
        }
    }
    closesocket(s);
    return sent;
}
#endif
