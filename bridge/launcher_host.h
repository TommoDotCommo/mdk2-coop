/* The launcher's hosting side (launcher_host.c): the lobby panel "Friends can join by" (C1), the router
 * port (C2), the Windows Firewall check, the invite (C3) and lobby visibility (B9), who may join (the
 * invite's secret, kicks), probe answers, the client's invite targets and the pad (K3).
 * launcher.c draws; this file finds out. MIT licence. */
#ifndef LAUNCHER_HOST_H
#define LAUNCHER_HOST_H
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define LHOST_JOIN_PAGE "tommodotcommo.github.io/join"

/* Settings > Network rows (coop.ini [network]) and the lobby's visibility. */
struct lhost_settings {
    int find_lan, find_tailscale;       /* joiner side: read by the Join page */
    int upnp;                           /* open the router port while hosting (C2, on by default) */
    int internet_in_invite;             /* put my internet address in invites */
    int invite_only;                    /* B9: 0 open to my networks, 1 invite only */
};
extern struct lhost_settings lhost_cfg;
void lhost_settings_load(const char *ini);
void lhost_settings_save(const char *ini);

/* the panel's rows, in the standard's order */
enum { LHOST_STEAM, LHOST_LAN, LHOST_TAILSCALE, LHOST_INTERNET, LHOST_FIREWALL, LHOST_ROWS };
enum { LHOST_CHECKING, LHOST_OK, LHOST_WARN, LHOST_OFF };
struct lhost_row { int state; char title[24], result[160], fix[200]; };

/* Host: open starts the checks on a worker thread (adapters, a probe of this PC's own lobby on its
 * LAN address, the firewall rules, Tailscale, the router). close(keep): keep=0 removes the router
 * mapping (Leave, quit); keep=1 keeps it renewed for the game this lobby starts. */
void lhost_open(unsigned port, int steam);
void lhost_close(int keep_mapping);
int lhost_rows(struct lhost_row *out);
/* C3: the invite text for the clipboard (and the code alone); 0 while the checks have no route yet. */
int lhost_invite(const char *name, char *message, size_t cap, char *code, size_t code_cap);
int lhost_copy_invite(HWND wnd, const char *name);
/* "router=<method> said=<text>", "firewall=..." lines for the test dump */
void lhost_dump(FILE *f);

/* Host, from the lobby socket: the reply to a CPQ1/CPQ2 probe (0: stay silent); who may join. */
int lhost_probe_answer(const uint8_t *q, int n, uint32_t from_ip, unsigned players, unsigned limit,
                       const char *version, const char *name, uint8_t *reply);
int lhost_admit(const char *hello, uint32_t ip, unsigned port, int members, int limit);   /* 0 or LH_REFUSE_* */
void lhost_kick(unsigned id, uint32_t ip, unsigned port, const char *name);
const char *lhost_refusal_text(int reason);
int lhost_refusal_parse(const char *s, int n);

/* Client: what the Join box holds. An invite (code or link) gives every route, its port and the
 * key; an address gives one target. Returns the count (0: not understood). */
#define LHOST_TARGETS 4
int lhost_join_targets(const char *text, unsigned default_port, struct sockaddr_in *to, uint32_t *key, int *has_key);
int lhost_hello(char *out, size_t cap, unsigned id, int ready, int rtt, unsigned long ts, const char *version,
                int has_key, uint32_t key, const char *name);

/* Pad (K3): actions since the last call (LH_ACT_*); pad_fake states count, so tests drive it. */
int lhost_pad_poll(DWORD now, int *actions, int cap);
int lhost_pad_step(unsigned buttons, int lx, int ly, DWORD now, int *actions, int cap);
#endif
