/* Bounded synthetic members for testing a real launcher's full-lobby status.
 * These are protocol peers, not game clients. Uses only the selected loopback port. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    WSADATA w;
    SOCKET s;
    struct sockaddr_in host = {0};
    DWORD until;
    int i, count, duration, game;
    if (argc != 5) return 2;
    game = !strcmp(argv[2], "game");
    count = atoi(argv[2]); duration = atoi(argv[4]);
    if (atoi(argv[1]) < 1 || atoi(argv[1]) > 65535 || (!game && (count < 1 || count > 15)) || duration < 1 || duration > 30000) return 2;
    if (WSAStartup(MAKEWORD(2,2), &w)) return 3;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 3;
    host.sin_family = AF_INET; host.sin_port = htons((u_short)atoi(argv[1]));
    host.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    until = GetTickCount() + duration;
    if (game) {
        unsigned char query[8] = {'C','P','Q','1',1,2,3,4}, reply[64];
        DWORD timeout = 1000;
        struct sockaddr_in from;
        int len = sizeof(from), n;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        sendto(s, (const char *)query, sizeof(query), 0, (struct sockaddr *)&host, sizeof(host));
        n = recvfrom(s, (char *)reply, sizeof(reply), 0, (struct sockaddr *)&from, &len);
        if (n != (int)(12 + strlen(argv[3])) || memcmp(reply, "CPS1", 4) || memcmp(reply+4, query+4, 4) ||
            from.sin_port != host.sin_port || from.sin_addr.s_addr != host.sin_addr.s_addr ||
            reply[8] != 3 || reply[10] != 1 || reply[11] != strlen(argv[3]) || memcmp(reply+12, argv[3], strlen(argv[3]))) return 5;
        puts("PASS: production game process answers probe with three connected players, in-game status and matching version");
        closesocket(s); WSACleanup(); return 0;
    }
    do {
        for (i = 0; i < count; i++) {
            char hello[180];
            int n = snprintf(hello, sizeof(hello), "MDKL H %x 1 0 %lu %s|Fixture%d", 0x70000000u + i,
                             (unsigned long)GetTickCount(), argv[3], i);
            if (sendto(s, hello, n, 0, (struct sockaddr *)&host, sizeof(host)) != n) return 4;
        }
        Sleep(100);
    } while ((LONG)(GetTickCount() - until) < 0);
    closesocket(s); WSACleanup();
    return 0;
}
