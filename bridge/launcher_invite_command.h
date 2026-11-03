/* Setup's per-user URL handler passes --coop-invite "<link>" to mdk2hd.exe.
 * Keep the link as paste-box text: opening a link never starts a join without confirmation.
 * Windows supplies argv through CommandLineToArgvW; this pure part is also tested without the game. */
#ifndef MDK2_LAUNCHER_INVITE_COMMAND_H
#define MDK2_LAUNCHER_INVITE_COMMAND_H
#include <stddef.h>
#include <wchar.h>
#define LI_INPUT_CAP 2049

/* 1: invite, 0: absent, -1: malformed; no truncated invites reach the paste box. */
static inline int li_invite_argument(int argc, const wchar_t *const *argv, char *out, size_t cap)
{
    int i, found = 0;
    size_t n, k;
    if (!out || !cap) return -1;
    out[0] = 0;
    for (i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--coop-invite")) continue;
        if (found || i + 1 == argc) { out[0] = 0; return -1; }
        n = wcslen(argv[++i]);
        if (!n || n >= cap || n >= LI_INPUT_CAP) { out[0] = 0; return -1; }
        for (k = 0; k < n; k++) {
            /* Invite codes and links are ASCII. Refuse controls and quotes, never reinterpret them as options. */
            if (argv[i][k] < 32 || argv[i][k] > 126 || argv[i][k] == L'"') { out[0] = 0; return -1; }
            out[k] = (char)argv[i][k];
        }
        out[n] = 0;
        found = 1;
    }
    return found;
}
#endif
