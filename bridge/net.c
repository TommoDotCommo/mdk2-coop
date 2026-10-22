/*
 * Network: UDP or Steam messages, star-shaped. The host (slot 0) talks to every client; a client talks to
 * the host only. Each sends its player state at send_hz in a player packet, which also
 * carries that link's reliable events and ack. The host passes each client's state on to
 * the other clients (relayed packets: state only). Entity snapshots have their own packet
 * (entities.c), host to clients.
 *
 * Clients get their slot from the host's packets ("you"). Every game copy has a player
 * id (coop\player.ini) and the host keeps each id's slot, so a player who drops out
 * gets the same number back. Every process has a random nonce: a new one means that game
 * restarted, and the link starts over; the host also numbers each link it opens (gen),
 * so a client whose link timed out on the host starts its side over too. A player silent
 * for PLAYER_TIMEOUT_MS has left.
 *
 * Level epochs: the host counts the level loads it announces; a client takes the epoch
 * from the LEVEL event once it is in that level. States and events from another epoch
 * are old-level leftovers and are dropped (a puppet drawn at its old-level spot wakes
 * that part of the new level).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include "bridge.h"

#define PLAYER_MAGIC    0x434b444d      /* "MDKC": FIRE includes finished projectile launch state */
#define PACKET_EVENTS   7      /* 7 x 128-byte events + player state fit in one datagram */
#define MAX_DATAGRAM    1400
#define PLAYER_TIMEOUT_MS 10000.0
#define NO_SLOT         0xff

#define PK_STATE        1       /* carries a player state */
#define PK_RELAY        2       /* host passing on another client's state: no ack, no events */
#define PK_NAME         4       /* relayed state carries the player's name (relay bundles) */

/* Relay bundle (host to client): the other players' latest states since the last bundle,
 * one datagram per client per send tick instead of one per relayed state. Header, then
 * count records: u8 slot, u8 flags, u8 level, u8 life, u32 id, u32 seq, u16 epoch,
 * i16 player_type, i16 hp, i16 hp_max, u8 reviving, u8 pad, u32 state time, [name, PLAYER_NAME bytes, if PK_NAME], u16 length,
 * snap_encode bytes. The name goes along about once a second. */
#define RELAY_MAGIC     0x534b444d      /* "MDKS": records with life and reviving */
struct relay_head {
    unsigned int magic;
    unsigned int nonce;         /* host process */
    unsigned short gen;         /* the receiver's link */
    unsigned char you;          /* the receiver's slot */
    unsigned char count;
};
#define RELAY_FIXED     (4 + 4 + 4 + 2 + 2 + 2 + 2 + 2 + 4)
#define RELAY_NAME_EVERY 30     /* states between names */

struct player_packet {
    unsigned int magic;
    unsigned int nonce;         /* sender process */
    unsigned int id;            /* sender's player id */
    unsigned short gen;         /* host to client: which link this is */
    unsigned short hold;        /* ms between receiving echo and sending this */
    unsigned int stamp;         /* sender's clock, ms (ping) */
    unsigned int echo;          /* the latest stamp the sender got on this link, 0 none */
    char name[PLAYER_NAME];     /* name of player `slot` */
    unsigned int seq;           /* state number of player `slot` */
    unsigned char slot;         /* whose state; NO_SLOT from a client that has no slot yet */
    unsigned char you;          /* host to client: the receiver's slot */
    unsigned char flags;
    unsigned char level;        /* level of the state; a state from another level is ignored */
    unsigned short epoch;
    short player_type;          /* OBJ type of that player (Kurt, Max, Doc, Hyde) */
    short hp, hp_max;           /* that player's hp, and the most it has had this level */
    unsigned char life;         /* LIFE_ALIVE / LIFE_DOWNED / LIFE_DEAD (revive.c) */
    unsigned char reviving;     /* slot + 1 of the downed player this one is reviving, 0 none */
    unsigned char pad2[2];
    struct snap state;
    unsigned int ack;           /* highest event id received in order on this link */
    unsigned char nevents;
    struct event events[PACKET_EVENTS];
};
#define PACKET_HEAD     offsetof(struct player_packet, events)
#define WIRE_HEAD       offsetof(struct player_packet, state)

/* On the wire the state is compact (snap_encode): the header up to `state`, u32 state time,
 * u16 state length (0 = no state), the encoded state, u32 ack, u8 nevents, the events. A full
 * struct snap went out with every packet before, ~390 bytes, mostly empty anim slots; the
 * host relays every player's state to every other client, so that was most of its upload. */
static int packet_pack(const struct player_packet *p, BYTE *out, int max)
{
    BYTE *o = out;
    int n = 0;

    unsigned short n16;

    if (max < (int)WIRE_HEAD + 4 + 2 + 4 + 1 + p->nevents * (int)sizeof(struct event))
        return 0;
    memcpy(o, p, WIRE_HEAD); o += WIRE_HEAD;
    memcpy(o, &p->state.time, 4); o += 4;
    if ((p->flags & PK_STATE) &&
        !(n = snap_encode(&p->state, o + 2, max - (int)(o + 2 - out) - 5 - p->nevents * (int)sizeof(struct event))))
        return 0;
    n16 = (unsigned short)n;
    memcpy(o, &n16, 2); o += 2 + n;
    memcpy(o, &p->ack, 4); o += 4;
    *o++ = p->nevents;
    memcpy(o, p->events, p->nevents * sizeof(struct event)); o += p->nevents * sizeof(struct event);
    return (int)(o - out);
}

/* 1 if buf is a whole, well-formed player packet; *p gets it in the in-memory form */
static int packet_unpack(const BYTE *buf, int len, struct player_packet *p)
{
    const BYTE *q = buf, *end = buf + len;
    int n;

    unsigned short n16;

    if (len < (int)WIRE_HEAD + 4 + 2)
        return 0;
    memset(p, 0, PACKET_HEAD);
    memcpy(p, q, WIRE_HEAD); q += WIRE_HEAD;
    if (p->magic != PLAYER_MAGIC)
        return 0;
    memcpy(&p->state.time, q, 4); q += 4;
    memcpy(&n16, q, 2); q += 2;
    n = n16;
    if (n && (end - q < n || snap_decode(q, n, &p->state) != n))
        return 0;
    q += n;
    if (end - q < 5)
        return 0;
    memcpy(&p->ack, q, 4); q += 4;
    p->nevents = *q++;
    if (p->nevents > PACKET_EVENTS || end - q != (int)(p->nevents * sizeof(struct event)))
        return 0;
    memcpy(p->events, q, p->nevents * sizeof(struct event));
    return 1;
}

/* send_to with the packet packed */
static void send_to(int slot, const void *buf, int len);
static void send_packet(int slot, const struct player_packet *p)
{
    BYTE wire[MAX_DATAGRAM];
    int len = packet_pack(p, wire, sizeof(wire));

    if (len)
        send_to(slot, wire, len);
}

/* settings from coop.ini */
int sync_anim = 1;              /* [sync] anim: 0 turns animation sync off */
int ent_hz = 15;                /* [sync] ent_hz: entity snapshots per second (host) */
static int send_hz = 30;        /* [sync] send_hz */
static int interp_ms = 100;     /* [sync] interp_ms: render the peer this far in the past; 0 = latest */
static int extrap_ms = 150;     /* [sync] extrap_ms: max extrapolation when packets are late */
static int sim_latency_ms;      /* [sim] latency_ms, jitter_ms, loss_pct: fake a bad network */
static int sim_jitter_ms;
static int sim_loss_pct;

int role_host = 1;              /* [net] role: host runs the world, clients mirror it */
unsigned int my_id;
char my_name[PLAYER_NAME];
int my_slot = -1;
int cur_level = -1;
int cur_cp;
unsigned short cur_epoch;
int have_remote;
static SOCKET sock = INVALID_SOCKET;
int net_steam;
BYTE *local_player;             /* last gob passed to coop_send */
BYTE *puppets[MAX_PLAYERS];

static unsigned int nonce;
static unsigned short pending_epoch;    /* client: from the last LEVEL event, taken on loading it */
static double block_until;              /* net_block: drop all traffic until then (tests) */
static unsigned int last_save_no;       /* host: newest co-op save, and the epoch it was made in */
static unsigned short last_save_epoch;
static int host_changed;                /* client: the host restarted since the last LEVEL */

/* ---- links: the host has one per client (by the client's slot), a client one to the host (0) ---- */

#define OUTBOX 256
#define INBOX  64

struct link {
    int used;
    struct sockaddr_in addr;
    uint64_t steam_id;
    unsigned int nonce;
    unsigned int id;                    /* host: the client's player id */
    unsigned short gen;
    unsigned int last_stamp;            /* their latest stamp, and when it came (ping) */
    double last_stamp_at;
    double ping;                        /* round trip, ms, smoothed; 0 unknown */
    double last_heard;
    struct event outbox[OUTBOX];
    unsigned int out_next, out_acked;   /* outbox holds ids out_acked+1 .. out_next-1 */
    struct event inbox[INBOX];
    int inbox_head, inbox_count;
    unsigned int in_next;               /* next event id expected */
};

static struct link links[MAX_PLAYERS];
static unsigned int slot_id[MAX_PLAYERS];  /* host: player id each slot belongs to, kept after they leave */
static uint64_t slot_steam_id[MAX_PLAYERS];
static unsigned short next_gen = 1;
static void slots_load(void);
static void player_left(int slot, const char *why);

/* ---- other players, by slot ---- */

struct player {
    int connected;
    double last_heard;
    unsigned int id;
    char name[PLAYER_NAME];
    int level;                  /* where their latest packet says they are */
    unsigned short epoch;
    unsigned int seq;           /* latest state number */
    int type;                   /* OBJ type of their character, 0 if unknown */
    int hp, hp_max;
    int life, reviving;         /* revive.c: LIFE_*, slot + 1 they are reviving */
    double clock_offset;        /* their clock - ours, least-delayed estimate */
    int have_clock;
    struct track track;
    struct {                    /* how evenly their puppet moves from frame to frame */
        float last[3];
        int have_last, frames, stalls;
        double sum, sumsq, max, window_start;
    } smooth;
};

static struct player players[MAX_PLAYERS];

/* host: latest state of each client still to be relayed, and to whom */
static struct {
    struct player_packet st;    /* header and state only */
    unsigned int to;            /* bit per slot: not yet sent there */
    unsigned int count;         /* states relayed so far, for the name */
} relay_pending[MAX_PLAYERS];

double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void link_reset(struct link *l)
{
    l->out_next = 1;
    l->out_acked = 0;
    l->inbox_head = l->inbox_count = 0;
    l->in_next = 1;
}

/* This copy's player id: made once, kept in coop\player.ini (install.sh rewrites coop.ini) */
static void load_player_id(void)
{
    char path[MAX_PATH], buf[16];

    snprintf(path, MAX_PATH, "%s\\player.ini", coop_dir);
    GetPrivateProfileStringA("player", "id", "", buf, sizeof(buf), path);
    my_id = strtoul(buf, NULL, 16);
    GetPrivateProfileStringA("player", "name", "", my_name, sizeof(my_name), path);
    if (!my_id) {
        my_id = (nonce * 2654435761u) ^ GetTickCount();
        if (!my_id)
            my_id = 1;
        snprintf(buf, sizeof(buf), "%08x", my_id);
        WritePrivateProfileStringA("player", "id", buf, path);
    }
    if (!my_name[0])
        snprintf(my_name, sizeof(my_name), "Player %04x", my_id & 0xffff);
    logmsg("player id %08x, name %s", my_id, my_name);
}

void net_open(const char *ini)
{
    char host[64], role[16], transport[16];
    int listen_port, peer_port;
    struct sockaddr_in local;
    u_long nonblock = 1;
    DWORD no_reset = FALSE, ret;
    WSADATA wsa;

    GetPrivateProfileStringA("net", "transport", "udp", transport, sizeof(transport), ini);
    net_steam = _stricmp(transport, "steam") == 0;
    listen_port = GetPrivateProfileIntA("net", "listen_port", 27900, ini);
    peer_port = GetPrivateProfileIntA("net", "peer_port", 27900, ini);
    GetPrivateProfileStringA("net", "peer_host", "127.0.0.1", host, sizeof(host), ini);
    GetPrivateProfileStringA("net", "role", "host", role, sizeof(role), ini);
    role_host = _stricmp(role, "client") != 0;
    sync_anim = GetPrivateProfileIntA("sync", "anim", 1, ini);
    send_hz = GetPrivateProfileIntA("sync", "send_hz", 30, ini);
    ent_hz = GetPrivateProfileIntA("sync", "ent_hz", 15, ini);
    interp_ms = GetPrivateProfileIntA("sync", "interp_ms", 100, ini);
    extrap_ms = GetPrivateProfileIntA("sync", "extrap_ms", 150, ini);
    sim_latency_ms = GetPrivateProfileIntA("sim", "latency_ms", 0, ini);
    sim_jitter_ms = GetPrivateProfileIntA("sim", "jitter_ms", 0, ini);
    sim_loss_pct = GetPrivateProfileIntA("sim", "loss_pct", 0, ini);
    if (send_hz < 1)
        send_hz = 1;
    if (ent_hz < 1)
        ent_hz = 1;
    logmsg("sync: send %d Hz, ents %d Hz, interp %d ms, extrap %d ms, anim %d; sim: latency %d ms, jitter %d ms, loss %d%%",
           send_hz, ent_hz, interp_ms, extrap_ms, sync_anim, sim_latency_ms, sim_jitter_ms, sim_loss_pct);
    nonce = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ (DWORD)(now_ms() * 1000.0);
    load_player_id();
    if (role_host) {
        my_slot = 0;
        /* a restarted host must not reuse the epochs of its last run: clients still in that
         * level would take its new loads for the one they are in */
        cur_epoch = (unsigned short)(nonce & 0xfff0);
        slots_load();
    }

    if (net_steam) {
        steam_open(ini);
        return;
    }
    if (_stricmp(transport, "udp") != 0) {
        logmsg("unknown transport %s: choose steam or udp in coop.ini", transport);
        return;
    }
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
    /* a datagram to a game that has quit comes back as ICMP port unreachable, which Windows
     * reports on the next recv (WSAECONNRESET): with several clients, one gone would cut every
     * frame's reading short. SIO_UDP_CONNRESET = _WSAIOW(IOC_VENDOR, 12) */
    WSAIoctl(sock, 0x9800000c, &no_reset, sizeof(no_reset), NULL, 0, &ret, NULL, NULL);

    if (!role_host) {
        struct link *l = &links[0];
        l->used = 1;
        l->addr.sin_family = AF_INET;
        l->addr.sin_port = htons((u_short)peer_port);
        l->addr.sin_addr.s_addr = inet_addr(host);
        link_reset(l);
    }
    logmsg("udp listen %d, role %s%s%s:%d", listen_port, role_host ? "host" : "client",
           role_host ? "" : ", host at ", role_host ? "" : host, role_host ? 0 : peer_port);
}

static void send_to(int slot, const void *buf, int len)
{
    if (block_until > now_ms())
        return;
    if (net_ready() && links[slot].used) {
        const struct player_packet *p = buf;
        int kind = NS_OTHER;                            /* netstats.c */
        if (len >= (int)sizeof(unsigned int) && p->magic == PLAYER_MAGIC)
            kind = len >= (int)offsetof(struct player_packet, state) && (p->flags & PK_RELAY) ? NS_RELAY : NS_PLAYER;
        else if (len >= (int)sizeof(unsigned int) && p->magic == RELAY_MAGIC)
            kind = NS_RELAY;
        else if (ents_is_packet(buf, len))
            kind = NS_ENT;
        else if (voice_is_packet(buf, len))
            kind = NS_VOICE;
        int sent = net_steam ? steam_send(links[slot].steam_id, buf, len) :
            sendto(sock, (const char *)buf, len, 0, (struct sockaddr *)&links[slot].addr, sizeof(links[slot].addr)) == len;
        if (sent)
            netstats_sent(kind, len);
    }
}

void net_send_to(int slot, const void *buf, int len)
{
    if (slot >= 0 && slot < MAX_PLAYERS)
        send_to(slot, buf, len);
}

/* Host: to every client; client: to the host */
void net_send(const void *buf, int len)
{
    int s;

    for (s = 0; s < MAX_PLAYERS; s++)
        send_to(s, buf, len);
}

/* ---- players ---- */

int player_connected(int slot)
{
    return slot >= 0 && slot < MAX_PLAYERS && slot != my_slot && players[slot].connected;
}

/* that player's latest packet says it is in our level and epoch */
int player_in_level(int slot)
{
    return player_connected(slot) && players[slot].level == cur_level && players[slot].epoch == cur_epoch;
}

/* Client: the host's player id, and how long since anything came from the host */
unsigned int host_id(void)
{
    return role_host ? my_id : players[0].id;
}

double host_silent_ms(void)
{
    /* never heard (a new host address): as silent as can be */
    return links[0].last_heard ? now_ms() - links[0].last_heard : 1e9;
}

int host_level(void)
{
    return players[0].connected ? players[0].level : -1;
}

/* Client: the host restarted since the last LEVEL event (once) */
int host_changed_take(void)
{
    int c = host_changed;
    host_changed = 0;
    return c;
}

/* A player's name ("" unknown) and ping in ms (0 unknown): the host measures every client,
 * a client only the host */
const char *player_name(int slot)
{
    if (slot == my_slot)
        return my_name;
    return player_connected(slot) ? players[slot].name : "";
}

/* A player's latest hp and highest hp this level (0, 0 unknown) */
void player_health(int slot, int *hp, int *hp_max)
{
    *hp = player_connected(slot) ? players[slot].hp : 0;
    *hp_max = player_connected(slot) ? players[slot].hp_max : 0;
}

/* revive.c: LIFE_* of that player (alive if unknown), and the slot they are reviving or -1 */
int player_life(int slot)
{
    return player_connected(slot) ? players[slot].life : LIFE_ALIVE;
}

int player_reviving(int slot)
{
    return player_connected(slot) ? players[slot].reviving - 1 : -1;
}

int player_ping(int slot)
{
    if (slot == my_slot || !player_connected(slot))
        return 0;
    if (role_host)
        return links[slot].used ? (int)(links[slot].ping + 0.5) : 0;
    return slot == 0 ? (int)(links[0].ping + 0.5) : 0;
}

/* Our name: printable ASCII, kept in coop\player.ini */
void net_set_name(const char *name)
{
    char path[MAX_PATH];
    int i, n = 0;

    for (i = 0; name[i] && n < PLAYER_NAME - 1; i++)
        if (name[i] >= 32 && name[i] < 127)
            my_name[n++] = name[i];
    my_name[n] = 0;
    if (!n)
        snprintf(my_name, sizeof(my_name), "Player %04x", my_id & 0xffff);
    snprintf(path, MAX_PATH, "%s\\player.ini", coop_dir);
    WritePrivateProfileStringA("player", "name", my_name, path);
    logmsg("name now %s", my_name);
}

/* Client: talk to the host at a new address from now on (the menu's Host address) */
int net_retarget(const char *host, int port)
{
    struct link *l = &links[0];
    unsigned long a = inet_addr(host);
    int s;

    if (net_steam || role_host || a == INADDR_NONE || port <= 0 || port > 65535)
        return 0;
    l->addr.sin_family = AF_INET;
    l->addr.sin_port = htons((u_short)port);
    l->addr.sin_addr.s_addr = a;
    l->nonce = 0;
    l->last_heard = 0;
    link_reset(l);
    for (s = 0; s < MAX_PLAYERS; s++)
        player_left(s, "new host address");
    my_slot = -1;
    logmsg("host address now %s:%d", host, port);
    return 1;
}

/* Tests: cut this game off the network for ms */
void net_block(int ms)
{
    block_until = now_ms() + ms;
    logmsg("network blocked for %d ms (test)", ms);
}

/* Host: the newest co-op save, offered to players who join in this epoch */
void net_set_last_save(unsigned int no)
{
    last_save_no = no;
    last_save_epoch = cur_epoch;
}

int player_type(int slot)
{
    return player_connected(slot) ? players[slot].type : 0;
}

int players_connected(void)
{
    int s, n = 0;

    for (s = 0; s < MAX_PLAYERS; s++)
        n += player_connected(s);
    return n;
}

static void player_left(int slot, const char *why)
{
    struct player *p = &players[slot];

    if (p->connected)
        logmsg("player %d left (%s)", slot + 1, why);
    memset(p, 0, sizeof(*p));
}

static void smooth_track(struct player *pl, int slot, const float *pos);

/* A player's interpolated state, NULL while there is none in this level; update: work
 * it out for this frame (once per frame), else the one worked out last */
struct snap *player_view(int slot, int update)
{
    struct player *p;

    if (!player_connected(slot) || !(p = &players[slot])->track.n)
        return NULL;
    if (update) {
        track_update(&p->track, p->clock_offset);
        smooth_track(p, slot, p->track.view.pos);
    }
    return &p->track.view;
}

/* entities.c: snapshots of enemies use the host's clock */
double host_clock_offset(void)
{
    return players[0].clock_offset;
}

/* ---- reliable events: per link, an outbox resent until acked and an inbox read in order ---- */

void event_init(struct event *e, int kind)
{
    memset(e, 0, sizeof(*e));
    e->kind = (unsigned char)kind;
    e->epoch = cur_epoch;
}

static void event_put(struct link *l, const struct event *e)
{
    struct event *o;

    if (!l->used)
        return;
    if (l->out_next - l->out_acked > OUTBOX) {
        logmsg("event outbox for player %d full, dropping kind %d", (int)(l - links) + 1, e->kind);
        return;
    }
    o = &l->outbox[l->out_next % OUTBOX];
    *o = *e;
    o->id = l->out_next++;
}

/* to: a player's slot, or TO_ALL (a client's only link is the host's) */
void event_send(const struct event *e, int to)
{
    int s;

    if (to >= 0 && to < MAX_PLAYERS) {
        event_put(&links[to], e);
        return;
    }
    for (s = 0; s < MAX_PLAYERS; s++)
        event_put(&links[s], e);
}

/* Host: pass an event from one client on to the others */
void event_relay(const struct event *e, int from)
{
    int s;

    if (!role_host)
        return;
    for (s = 1; s < MAX_PLAYERS; s++)
        if (s != from)
            event_put(&links[s], e);
}

void event_queue(int kind, const char *name, int damage, int type, int part)
{
    struct event e;

    event_init(&e, kind);
    e.damage = (short)damage;
    e.type = type;
    e.part = (signed char)part;
    snprintf(e.name, EV_NAME, "%s", name);
    event_send(&e, TO_ALL);
}

/* hp >= 0: the target's hp after the hit on the host, for the clients' mirror (APPLY only) */
static void event_queue_hit(int to, int kind, const char *name, BYTE *from, int damage, int type, int part, int hp)
{
    struct event e;

    event_init(&e, kind);
    e.damage = (short)damage;
    e.type = type;
    e.part = (signed char)part;
    e.extra[0] = (DWORD)(hp + 1);       /* 0: not sent */
    snprintf(e.name, EV_NAME, "%s", name);
    gob_peername(from, e.from, EV_NAME);
    event_send(&e, to);
}

void event_queue_from(int to, int kind, const char *name, BYTE *from, int damage, int type, int part)
{
    event_queue_hit(to, kind, name, from, damage, type, part, -1);
}

void event_queue_applied(const char *name, BYTE *from, int damage, int type, int part, int hp)
{
    event_queue_hit(TO_ALL, EV_APPLY, name, from, damage, type, part, hp);
}

/* SPAWN: an enemy with its current hp (a late joiner's copy starts fresh) */
void event_queue_spawn(int to, int kind, unsigned int netid, int objtype, int hp, const float *pos, const char *name)
{
    struct event e;

    event_init(&e, kind);
    e.netid = netid;
    e.damage = (short)hp;
    e.objtype = (short)objtype;
    if (pos)
        memcpy(e.pos, pos, sizeof(e.pos));
    snprintf(e.name, EV_NAME, "%s", name);
    event_send(&e, to);
}

/* next event from any link; *from = the sender's slot */
int event_next(struct event *out, int *from)
{
    static int next;
    int i;

    for (i = 0; i < MAX_PLAYERS; i++) {
        struct link *l = &links[(next + i) % MAX_PLAYERS];
        if (!l->inbox_count)
            continue;
        *out = l->inbox[l->inbox_head];
        l->inbox_head = (l->inbox_head + 1) % INBOX;
        l->inbox_count--;
        *from = (int)(l - links);
        next = (*from + 1) % MAX_PLAYERS;
        return 1;
    }
    return 0;
}

/* Put back the event event_next just returned from that link: it is read again next time */
void event_unget(int from)
{
    struct link *l = &links[from];

    l->inbox_head = (l->inbox_head + INBOX - 1) % INBOX;
    l->inbox_count++;
}

/* Client: an event that has to wait. A LEVEL event waits until the game has reached the
 * title screen (followed during the intro, the intro's own title load lands on top of the
 * level). Events of a level we are loading wait until it has loaded: the load finishes a
 * few frames after mdkNewGame returns, and enemy SPAWNs read before that were dropped. */
int event_must_wait(const struct event *e)
{
    if (role_host)
        return 0;
    if (e->kind == EV_LEVEL)
        return cur_level < 0;
    return cur_epoch != pending_epoch && e->epoch == pending_epoch;
}

/* Ping on a direct link: they echo our latest stamp and how long they held it */
static void link_ping(struct link *l, const struct player_packet *p, double arrived)
{
    l->last_stamp = p->stamp;
    l->last_stamp_at = arrived;
    if (p->echo) {
        /* our clock is a double, the stamp its low 32 bits: compare in the same width */
        double rtt = (double)(unsigned int)((unsigned int)arrived - p->echo) - p->hold;
        if (rtt >= 0.0 && rtt < 10000.0)
            l->ping = l->ping > 0.0 ? l->ping * 0.8 + rtt * 0.2 : rtt;
    }
}

static void events_receive(struct link *l, const struct player_packet *p)
{
    int i;

    if (p->ack > l->out_acked && p->ack < l->out_next)
        l->out_acked = p->ack;
    for (i = 0; i < p->nevents; i++) {
        if (p->events[i].id != l->in_next || l->inbox_count == INBOX)
            continue;               /* duplicate, or a gap: wait for the resend */
        l->inbox[(l->inbox_head + l->inbox_count) % INBOX] = p->events[i];
        l->inbox_count++;
        l->in_next++;
    }
}

/* ---- snapshots and interpolation ---- */

void track_add(struct track *t, const struct snap *s)
{
    int i;

    if (t->n == TRACK_SNAPS) {
        memmove(t->snaps, t->snaps + 1, sizeof(t->snaps[0]) * (TRACK_SNAPS - 1));
        t->n--;
    }
    for (i = t->n; i > 0 && (int)(t->snaps[i - 1].time - s->time) > 0; i--)
        t->snaps[i] = t->snaps[i - 1];
    t->snaps[i] = *s;
    t->n++;
}

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
static void blend(struct snap *out, const struct snap *a, const struct snap *b, float t)
{
    const struct snap *other = t < 0.5f ? b : a;
    int i, j, k;

    *out = t < 0.5f ? *a : *b;
    for (k = 0; k < 3; k++)
        out->pos[k] = lerpf(a->pos[k], b->pos[k], t);
    nlerp_quat(out->quat, a->quat, b->quat, t);
    for (i = 0; i < out->nanims; i++) {
        const float *sa, *sb;
        float *d;

        for (j = 0; j < other->nanims && other->anims[j].id != out->anims[i].id; j++)
            ;
        if (j == other->nanims)
            continue;
        sa = (const float *)(t < 0.5f ? out->anims[i].state : other->anims[j].state);
        sb = (const float *)(t < 0.5f ? other->anims[j].state : out->anims[i].state);
        d = (float *)out->anims[i].state;
        d[ANIM_TIME0] = lerp_phase(sa[ANIM_TIME0], sb[ANIM_TIME0], t);
        d[ANIM_TIME1] = lerp_phase(sa[ANIM_TIME1], sb[ANIM_TIME1], t);
        d[ANIM_WEIGHT] = lerpf(sa[ANIM_WEIGHT], sb[ANIM_WEIGHT], t);
    }
}

void track_update(struct track *t, double clock_offset)
{
    double render;
    int i;

    if (!t->n)
        return;
    if (interp_ms <= 0 || t->n == 1) {
        t->view = t->snaps[t->n - 1];
        return;
    }
    render = now_ms() + clock_offset - interp_ms;
    for (i = t->n - 1; i > 0 && (double)t->snaps[i - 1].time > render; i--)
        ;
    /* snaps[i-1] .. snaps[i] bracket render, or render is outside the buffer */
    if (i == 0) {
        t->view = t->snaps[0];
    } else if ((double)t->snaps[i].time >= render) {
        const struct snap *a = &t->snaps[i - 1], *b = &t->snaps[i];
        float span = (float)(b->time - a->time);
        blend(&t->view, a, b, span > 0.0f ? (float)(render - a->time) / span : 1.0f);
    } else {
        /* newest snapshot is older than render time: extrapolate along the last step */
        const struct snap *a = &t->snaps[t->n - 2], *b = &t->snaps[t->n - 1];
        float span = (float)(b->time - a->time);
        double ahead = render - b->time;
        int k;

        if (ahead > extrap_ms)
            ahead = extrap_ms;
        t->view = *b;
        if (span > 0.0f)
            for (k = 0; k < 3; k++)
                t->view.pos[k] = b->pos[k] + (b->pos[k] - a->pos[k]) * (float)(ahead / span);
    }
}

/* ---- animation state ---- */

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

void snap_capture(BYTE *gob, struct snap *s)
{
    BYTE *ctl = *(BYTE **)(gob + GOB_ANIM);
    int id;

    s->time = (unsigned int)now_ms();
    memcpy(s->pos, gob + GOB_POSITION, sizeof(s->pos));
    memcpy(s->quat, gob + GOB_ORIENTATION, sizeof(s->quat));
    s->cur = s->prev = NO_ANIM;
    s->nanims = 0;
    if (!ctl)
        return;
    s->cur = anim_id_of(gob, *(DWORD **)(ctl + 0x28));
    s->prev = anim_id_of(gob, *(DWORD **)(ctl + 0x2c));
    for (id = 0; id < ANIM_ID_MAX && s->nanims < SNAP_ANIMS; id++) {
        DWORD *st = om_anim_state(gob, id);
        if (st && st[0]) {
            s->anims[s->nanims].id = (unsigned char)id;
            memcpy(s->anims[s->nanims].state, st, sizeof(s->anims[0].state));
            s->nanims++;
        }
    }
}

/* ---- compact snapshots on the wire ----
 * A struct snap is ~390 bytes, almost all of it the 8 anim slots; most enemies have no
 * anim running (rooms in stasis) and a running anim is mostly zeros and constants with
 * only its play position moving. Wire form (little endian, no padding):
 *   pos 3 x float, quat 4 x int16 (x 32767), cur, prev, nanims, then per anim:
 *   id, u16 mask (bit k: state[k] is non-zero and follows; bit 15: [3] [4] [6] are in 0..1
 *   and follow as u16 x 65535), the non-zero state DWORDs.
 * The time is not in it: callers carry one per packet. */

#define SNAP_QUANT      0x8000
#define QUANT_FIELD(k)  ((k) == ANIM_TIME0 || (k) == ANIM_TIME1 || (k) == ANIM_WEIGHT)

static int unit_float(DWORD d)
{
    float f;
    memcpy(&f, &d, 4);
    return f >= 0.0f && f <= 1.0f;
}

/* bytes written, or 0 if it does not fit in max */
int snap_encode(const struct snap *s, BYTE *out, int max)
{
    BYTE *o = out, *end = out + max;
    int i, k;

    if (end - o < 12 + 8 + 3)
        return 0;
    memcpy(o, s->pos, 12); o += 12;
    for (k = 0; k < 4; k++) {
        float q = s->quat[k] < -1.0f ? -1.0f : s->quat[k] > 1.0f ? 1.0f : s->quat[k];
        short v = (short)lrintf(q * 32767.0f);
        memcpy(o, &v, 2); o += 2;
    }
    *o++ = s->cur;
    *o++ = s->prev;
    *o++ = s->nanims;
    for (i = 0; i < s->nanims; i++) {
        const DWORD *st = s->anims[i].state;
        unsigned short mask = 0;
        int quant = 1;

        for (k = 0; k < ANIM_STATE_DWORDS; k++) {
            if (st[k])
                mask |= (unsigned short)(1 << k);
            if (QUANT_FIELD(k) && st[k] && !unit_float(st[k]))
                quant = 0;
        }
        if (quant)
            mask |= SNAP_QUANT;
        if (end - o < 3 + ANIM_STATE_DWORDS * 4)
            return 0;
        *o++ = s->anims[i].id;
        memcpy(o, &mask, 2); o += 2;
        for (k = 0; k < ANIM_STATE_DWORDS; k++) {
            if (!(mask & (1 << k)))
                continue;
            if (quant && QUANT_FIELD(k)) {
                float f;
                unsigned short v;
                memcpy(&f, &st[k], 4);
                v = (unsigned short)lrintf(f * 65535.0f);
                memcpy(o, &v, 2); o += 2;
            } else {
                memcpy(o, &st[k], 4); o += 4;
            }
        }
    }
    return (int)(o - out);
}

/* bytes read, or 0 if the data is short or bad; s->time is left alone */
int snap_decode(const BYTE *in, int len, struct snap *s)
{
    const BYTE *p = in, *end = in + len;
    float n = 0.0f;
    int i, k;

    if (end - p < 12 + 8 + 3)
        return 0;
    memcpy(s->pos, p, 12); p += 12;
    for (k = 0; k < 4; k++) {
        short v;
        memcpy(&v, p, 2); p += 2;
        s->quat[k] = v / 32767.0f;
        n += s->quat[k] * s->quat[k];
    }
    n = n > 0.0f ? 1.0f / sqrtf(n) : 1.0f;
    for (k = 0; k < 4; k++)
        s->quat[k] *= n;
    s->cur = *p++;
    s->prev = *p++;
    s->nanims = *p++;
    if (s->nanims > SNAP_ANIMS)
        return 0;
    for (i = 0; i < s->nanims; i++) {
        unsigned short mask;
        DWORD *st = s->anims[i].state;

        if (end - p < 3)
            return 0;
        s->anims[i].id = *p++;
        memcpy(&mask, p, 2); p += 2;
        for (k = 0; k < ANIM_STATE_DWORDS; k++) {
            st[k] = 0;
            if (!(mask & (1 << k)))
                continue;
            if ((mask & SNAP_QUANT) && QUANT_FIELD(k)) {
                unsigned short v;
                float f;
                if (end - p < 2)
                    return 0;
                memcpy(&v, p, 2); p += 2;
                f = v / 65535.0f;
                memcpy(&st[k], &f, 4);
            } else {
                if (end - p < 4)
                    return 0;
                memcpy(&st[k], p, 4); p += 4;
            }
        }
    }
    return (int)(p - in);
}

/* Make gob's anim states match the snapshot: copy active blocks, stop the rest */
void anim_apply(BYTE *gob, const struct snap *s)
{
    BYTE *ctl = *(BYTE **)(gob + GOB_ANIM);
    int id, i;

    if (!ctl)
        return;
    for (id = 0; id < ANIM_ID_MAX; id++) {
        DWORD *st = om_anim_state(gob, id);
        if (!st)
            continue;
        for (i = 0; i < s->nanims && s->anims[i].id != id; i++)
            ;
        if (i < s->nanims)
            memcpy(st, s->anims[i].state, sizeof(s->anims[0].state));
        else
            st[0] = 0;
    }
    *(DWORD **)(ctl + 0x28) = s->cur != NO_ANIM ? om_anim_state(gob, s->cur) : NULL;
    *(DWORD **)(ctl + 0x2c) = s->prev != NO_ANIM ? om_anim_state(gob, s->prev) : NULL;
}

/* ---- player packets ---- */

static void relay_flush(void);

/* Our state to every link, each with its own ack and events */
void net_send_player(BYTE *gob)
{
    static double next_send;
    static unsigned int seq;
    struct player_packet p;
    double now = now_ms();
    unsigned int id;
    int s;

    if (!net_ready() || now < next_send)
        return;
    next_send = (now - next_send > 1000.0 / send_hz) ? now + 1000.0 / send_hz : next_send + 1000.0 / send_hz;
    if (role_host)
        relay_flush();
    memset(&p, 0, PACKET_HEAD);
    p.magic = PLAYER_MAGIC;
    p.nonce = nonce;
    p.id = my_id;
    memcpy(p.name, my_name, PLAYER_NAME);
    p.stamp = (unsigned int)now;
    p.seq = ++seq;
    p.slot = my_slot >= 0 ? (unsigned char)my_slot : NO_SLOT;
    p.level = (unsigned char)cur_level;
    p.epoch = cur_epoch;
    if (gob) {
        p.flags = PK_STATE;
        p.player_type = (short)gob_type(gob);
        p.hp = (short)gob_hp(gob);
        p.hp_max = (short)(local_max_hp > p.hp ? local_max_hp : p.hp);
        p.life = (unsigned char)revive_life();
        p.reviving = (unsigned char)(revive_reviving() + 1);
        snap_capture(gob, &p.state);
    }
    for (s = 0; s < MAX_PLAYERS; s++) {
        struct link *l = &links[s];
        if (!l->used)
            continue;
        p.you = (unsigned char)s;
        p.gen = l->gen;
        p.echo = l->last_stamp;
        p.hold = l->last_stamp ? (unsigned short)(now - l->last_stamp_at) : 0;
        p.ack = l->in_next - 1;
        p.nevents = 0;
        for (id = l->out_acked + 1; id < l->out_next && p.nevents < PACKET_EVENTS; id++)
            p.events[p.nevents++] = l->outbox[id % OUTBOX];
        send_packet(s, &p);
        if (p.seq % 900 == 0)
            logmsg("send #%u to player %d pos (%.2f, %.2f, %.2f) anim cur %d n %d, events %d unacked", p.seq, s + 1,
                   p.state.pos[0], p.state.pos[1], p.state.pos[2], p.state.cur, p.state.nanims,
                   l->out_next - 1 - l->out_acked);
    }
}

static void clock_sample(struct player *pl, unsigned int sender_time, double arrived)
{
    double o = (double)sender_time - arrived;

    if (!pl->have_clock || o > pl->clock_offset)
        pl->clock_offset = o;
    else
        pl->clock_offset -= 0.01;      /* let the estimate follow slow clock drift */
    pl->have_clock = 1;
}

/* A state of another player, direct or relayed */
static void accept_state(int slot, const struct player_packet *p, double arrived)
{
    struct player *pl = &players[slot];

    if (slot == my_slot || slot >= MAX_PLAYERS)
        return;
    pl->id = p->id;
    if (!(p->flags & PK_RELAY) || (p->flags & PK_NAME)) {     /* bundles carry it now and then */
        memcpy(pl->name, p->name, PLAYER_NAME);
        pl->name[PLAYER_NAME - 1] = 0;
    }
    pl->level = p->level;
    pl->epoch = p->epoch;
    if (!pl->connected) {
        logmsg("player %d connected", slot + 1);
        pl->connected = 1;
    } else if ((int)(p->seq - pl->seq) <= 0 && pl->seq - p->seq < 300) {
        pl->last_heard = arrived;
        return;                         /* late or duplicate (a big step back is a restart) */
    }
    pl->last_heard = arrived;
    pl->seq = p->seq;
    if ((p->flags & PK_STATE) && p->level == (unsigned char)cur_level && p->epoch == cur_epoch &&
        p->state.nanims <= SNAP_ANIMS) {
        pl->type = p->player_type;
        pl->hp = p->hp;
        pl->hp_max = p->hp_max;
        pl->life = p->life;
        pl->reviving = p->reviving;
        clock_sample(pl, p->state.time, arrived);
        track_add(&pl->track, &p->state);
    }
}

/* Host: which player id had which slot, in coop\player.ini [slots]: a restarted host gives
 * everyone their old number back */
static void slots_load(void)
{
    char path[MAX_PATH], key[8], buf[24];
    int s;

    snprintf(path, MAX_PATH, "%s\\player.ini", coop_dir);
    for (s = 1; s < MAX_PLAYERS; s++) {
        snprintf(key, sizeof(key), "p%d", s + 1);
        GetPrivateProfileStringA("slots", key, "", buf, sizeof(buf), path);
        slot_id[s] = strtoul(buf, NULL, 16);
        GetPrivateProfileStringA("steam_slots", key, "0", buf, sizeof(buf), path);
        slot_steam_id[s] = strtoull(buf, NULL, 10);
    }
}

static void slot_keep(int s, unsigned int id, uint64_t steam_id)
{
    char path[MAX_PATH], key[8], buf[24];

    if (slot_id[s] == id && slot_steam_id[s] == steam_id)
        return;
    slot_steam_id[s] = steam_id;
    slot_id[s] = id;
    snprintf(path, MAX_PATH, "%s\\player.ini", coop_dir);
    snprintf(key, sizeof(key), "p%d", s + 1);
    snprintf(buf, sizeof(buf), "%08x", id);
    WritePrivateProfileStringA("slots", key, buf, path);
    if (steam_id) {
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)steam_id);
        WritePrivateProfileStringA("steam_slots", key, buf, path);
    }
}

/* Host: the slot for a player id: the one it had before if that is free, else a free one
 * nobody has had, else any free one */
static int slot_for(unsigned int id, uint64_t steam_id)
{
    int s;

    for (s = 1; s < MAX_PLAYERS; s++)
        if (!links[s].used && (net_steam ? slot_steam_id[s] == steam_id : slot_id[s] == id))
            return s;
    for (s = 1; s < MAX_PLAYERS; s++)
        if (!links[s].used && (net_steam ? !slot_steam_id[s] : !slot_id[s]))
            return s;
    for (s = 1; s < MAX_PLAYERS; s++)
        if (!links[s].used)
            return s;
    return -1;
}

/* Host: a new link (or a restarted game): bring it into the level we are in, with every
 * enemy there, the items already taken, and the newest co-op save of this level to load */
static void host_welcome(int s)
{
    struct link *l = &links[s];
    struct event e;

    link_reset(l);
    l->gen = next_gen++;
    rules_fill(&e);
    event_put(l, &e);
    if (cur_level < 1)
        return;
    event_init(&e, EV_LEVEL);
    e.damage = (short)cur_cp;
    e.type = cur_level;
    e.netid = cur_epoch;
    e.extra[0] = last_save_epoch == cur_epoch ? last_save_no : 0;
    e.extra[1] = my_id;
    event_put(l, &e);
    ents_resend(s);
    pickups_resend(s);
    logmsg("player %d asked to join level %d checkpoint %d (save %u)", s + 1, cur_level, cur_cp,
           (unsigned)e.extra[0]);
}

/* Host: a client's packet; its link is found by player id (else address), or made */
static void host_accept(const struct player_packet *p, const struct sockaddr_in *from, uint64_t steam_id, double arrived)
{
    struct link *l = NULL;
    int s;

    for (s = 1; s < MAX_PLAYERS && !l; s++)
        if (links[s].used && (net_steam ? links[s].steam_id == steam_id : links[s].id == p->id))
            l = &links[s];
    for (s = 1; !net_steam && s < MAX_PLAYERS && !l; s++)
        if (links[s].used && links[s].addr.sin_addr.s_addr == from->sin_addr.s_addr &&
            links[s].addr.sin_port == from->sin_port)
            l = &links[s];
    if (l) {
        s = (int)(l - links);
        if (!net_steam && (l->addr.sin_addr.s_addr != from->sin_addr.s_addr || l->addr.sin_port != from->sin_port)) {
            logmsg("player %d now at %s:%d", s + 1, inet_ntoa(from->sin_addr), ntohs(from->sin_port));
            l->addr = *from;
        }
        if (l->nonce != p->nonce) {
            logmsg("player %d restarted its game", s + 1);
            player_left(s, "restarted");
            l->nonce = p->nonce;
            l->id = p->id;
            slot_keep(s, p->id, steam_id);
            host_welcome(s);
        }
    } else {
        if ((s = slot_for(p->id, steam_id)) < 0) {
            static double next_log;
            if (arrived > next_log) {
                logmsg("game full: ignoring new peer");
                next_log = arrived + 5000.0;
            }
            return;
        }
        l = &links[s];
        memset(l, 0, sizeof(*l));
        l->used = 1;
        l->addr = *from;
        l->steam_id = steam_id;
        l->nonce = p->nonce;
        l->id = p->id;
        if (net_steam)
            logmsg("player %d joined via Steam", s + 1);
        else
            logmsg("player %d joined from %s:%d (id %08x%s)", s + 1, inet_ntoa(from->sin_addr), ntohs(from->sin_port),
                   p->id, slot_id[s] == p->id ? ", back again" : "");
        slot_keep(s, p->id, steam_id);
        host_welcome(s);
    }
    l->last_heard = arrived;
    link_ping(l, p, arrived);
    events_receive(l, p);
    accept_state(s, p, arrived);
    have_remote = 1;

    /* pass the state on to the other clients, in the next relay bundle */
    if (p->flags & PK_STATE) {
        unsigned int to = 0;
        memcpy(&relay_pending[s].st, p, PACKET_HEAD);
        for (int d = 1; d < MAX_PLAYERS; d++)
            if (links[d].used && d != s)
                to |= 1u << d;
        relay_pending[s].to = to;
    }
}

/* Host: one bundle per client with the states that came in since the last one */
static void relay_flush(void)
{
    BYTE buf[MAX_DATAGRAM];
    int d, s;

    for (d = 1; d < MAX_PLAYERS; d++) {
        struct relay_head *h = (struct relay_head *)buf;
        BYTE *o = buf + sizeof(*h);

        if (!links[d].used)
            continue;
        h->magic = RELAY_MAGIC;
        h->nonce = nonce;
        h->gen = links[d].gen;
        h->you = (unsigned char)d;
        h->count = 0;
        for (s = 1; s < MAX_PLAYERS; s++) {
            const struct player_packet *p = &relay_pending[s].st;
            int name, n;
            unsigned short n16;
            BYTE flags;

            if (!(relay_pending[s].to & (1u << d)))
                continue;
            name = relay_pending[s].count % RELAY_NAME_EVERY == 0;
            if (buf + sizeof(buf) - o < RELAY_FIXED + PLAYER_NAME + 2 + 400) {
                send_to(d, buf, (int)(o - buf));
                o = buf + sizeof(*h);
                h->count = 0;
            }
            flags = (BYTE)((p->flags & PK_STATE) | PK_RELAY | (name ? PK_NAME : 0));
            *o++ = (BYTE)s;
            *o++ = flags;
            *o++ = p->level;
            *o++ = p->life;
            memcpy(o, &p->id, 4); o += 4;
            memcpy(o, &p->seq, 4); o += 4;
            memcpy(o, &p->epoch, 2); o += 2;
            memcpy(o, &p->player_type, 2); o += 2;
            memcpy(o, &p->hp, 2); o += 2;
            memcpy(o, &p->hp_max, 2); o += 2;
            *o++ = p->reviving;
            *o++ = 0;
            memcpy(o, &p->state.time, 4); o += 4;
            if (name) {
                memcpy(o, p->name, PLAYER_NAME);
                o += PLAYER_NAME;
            }
            n = snap_encode(&p->state, o + 2, 400);
            n16 = (unsigned short)n;
            memcpy(o, &n16, 2);
            o += 2 + n;
            h->count++;
            relay_pending[s].to &= ~(1u << d);
        }
        if (h->count)
            send_to(d, buf, (int)(o - buf));
    }
    for (s = 1; s < MAX_PLAYERS; s++)
        if (relay_pending[s].st.magic && !relay_pending[s].to) {
            relay_pending[s].count++;
            relay_pending[s].st.magic = 0;      /* sent everywhere */
        }
}

/* Client: a relay bundle from our host */
static void relay_receive(const BYTE *buf, int len, double arrived)
{
    const struct relay_head *h = (const struct relay_head *)buf;
    const BYTE *q = buf + sizeof(*h), *end = buf + len;
    struct link *l = &links[0];
    static struct player_packet p;
    int i;

    if (len < (int)sizeof(*h) || h->nonce != l->nonce || h->gen != l->gen)
        return;                 /* the host's own packets set up the link first */
    l->last_heard = arrived;
    for (i = 0; i < h->count; i++) {
        unsigned short n16;
        int slot;

        if (end - q < RELAY_FIXED + 2)
            return;
        memset(&p, 0, PACKET_HEAD);
        slot = q[0];
        p.flags = q[1];
        p.level = q[2];
        p.life = q[3];
        q += 4;
        memcpy(&p.id, q, 4); q += 4;
        memcpy(&p.seq, q, 4); q += 4;
        memcpy(&p.epoch, q, 2); q += 2;
        memcpy(&p.player_type, q, 2); q += 2;
        memcpy(&p.hp, q, 2); q += 2;
        memcpy(&p.hp_max, q, 2); q += 2;
        p.reviving = q[0];
        q += 2;
        memcpy(&p.state.time, q, 4); q += 4;
        if (p.flags & PK_NAME) {
            if (end - q < PLAYER_NAME + 2)
                return;
            memcpy(p.name, q, PLAYER_NAME);
            q += PLAYER_NAME;
        }
        memcpy(&n16, q, 2); q += 2;
        if (end - q < n16 || (n16 && snap_decode(q, n16, &p.state) != n16))
            return;
        q += n16;
        if (slot < MAX_PLAYERS)
            accept_state(slot, &p, arrived);
    }
}

/* Client: everything comes from the host */
static void client_accept(const struct player_packet *p, double arrived)
{
    struct link *l = &links[0];
    int s;

    if (l->nonce != p->nonce) {
        if (l->nonce) {
            logmsg("the host restarted its game");
            host_changed = 1;
        }
        for (s = 0; s < MAX_PLAYERS; s++)
            player_left(s, "host restarted");
        link_reset(l);
        l->nonce = p->nonce;
        l->gen = p->gen;
        my_slot = -1;
    } else if (l->gen != p->gen) {
        /* the host timed our link out and opened a new one: its event numbers start over */
        logmsg("the host opened a new link (%u -> %u): events start over", l->gen, p->gen);
        link_reset(l);
        l->gen = p->gen;
    }
    if (p->you != my_slot && p->you < MAX_PLAYERS) {
        my_slot = p->you;
        logmsg("we are player %d", my_slot + 1);
    }
    l->last_heard = arrived;
    if (p->flags & PK_RELAY) {
        accept_state(p->slot, p, arrived);
        return;
    }
    if (!have_remote)
        logmsg("first packet from peer (host, seq %u)", p->seq);
    have_remote = 1;
    link_ping(l, p, arrived);
    events_receive(l, p);
    accept_state(0, p, arrived);
}

static void accept_datagram(const BYTE *buf, int len, const struct sockaddr_in *from, uint64_t steam_id, double arrived)
{
    static struct player_packet unpacked;
    const struct player_packet *p = &unpacked;

    if (len >= (int)sizeof(unsigned int) && *(const unsigned int *)buf == PLAYER_MAGIC) {
        if (!packet_unpack(buf, len, &unpacked))
            return;
        if (role_host)
            host_accept(p, from, steam_id, arrived);
        else if (net_steam ? steam_id == links[0].steam_id :
                 (from->sin_addr.s_addr == links[0].addr.sin_addr.s_addr && from->sin_port == links[0].addr.sin_port))
            client_accept(p, arrived);      /* only the host we are set to (Options > Co-op > Host address) */
    } else if (!role_host && len >= (int)sizeof(unsigned int) && *(const unsigned int *)buf == RELAY_MAGIC) {
        if (net_steam ? steam_id == links[0].steam_id :
            (from->sin_addr.s_addr == links[0].addr.sin_addr.s_addr && from->sin_port == links[0].addr.sin_port))
            relay_receive(buf, len, arrived);
    } else if (voice_is_packet(buf, len)) {
        int slot = -1, s;

        if (role_host) {
            for (s = 0; s < MAX_PLAYERS; s++)
                if (links[s].used && (net_steam ? links[s].steam_id == steam_id :
                    (links[s].addr.sin_addr.s_addr == from->sin_addr.s_addr && links[s].addr.sin_port == from->sin_port)))
                    slot = s;
        } else if (net_steam ? steam_id == links[0].steam_id :
                   (from->sin_addr.s_addr == links[0].addr.sin_addr.s_addr && from->sin_port == links[0].addr.sin_port)) {
            slot = 0;
        }
        if (slot >= 0 && have_remote)
            voice_receive(slot, buf, len);
    } else if (have_remote && (role_host || (net_steam ? steam_id == links[0].steam_id :
                (from->sin_addr.s_addr == links[0].addr.sin_addr.s_addr && from->sin_port == links[0].addr.sin_port)))) {
        ents_receive(buf, len);
    }
}

/* Drop players and links that have gone quiet */
static void check_timeouts(double now)
{
    int s, any = 0;

    for (s = 0; s < MAX_PLAYERS; s++) {
        if (players[s].connected && now - players[s].last_heard > PLAYER_TIMEOUT_MS)
            player_left(s, "silent for 10 s");
        if (role_host && s > 0 && links[s].used && now - links[s].last_heard > PLAYER_TIMEOUT_MS) {
            links[s].used = 0;
            player_left(s, "silent for 10 s");
        }
        any |= players[s].connected;
    }
    have_remote = any;
}

/* New level: old states of the others would make their puppets jump. Announcing it (the
 * host) starts a new epoch; a client is in the epoch of the LEVEL event it followed. */
void net_level_reset(int announce)
{
    int s;

    if (role_host && announce)
        cur_epoch++;
    else if (!role_host)
        cur_epoch = pending_epoch;
    for (s = 0; s < MAX_PLAYERS; s++) {
        players[s].track.n = 0;
        players[s].type = 0;
        memset(&players[s].smooth, 0, sizeof(players[s].smooth));
    }
}

/* Client: the host's LEVEL event (level in type, checkpoint in damage, epoch in netid).
 * Only the completed load adopts a new epoch. Matching checkpoint numbers can
 * still describe different worlds; adopting early would apply new spawns to old gobs.
 * A same-epoch reconnect already has this epoch and needs no load. */
void net_level_event(const struct event *e)
{
    pending_epoch = (unsigned short)e->netid;
}

/* ---- simulated network: hold datagrams back, drop some ---- */

#define SIM_QUEUE 512

static struct {
    BYTE buf[MAX_DATAGRAM];
    int len;
    struct sockaddr_in from;
    uint64_t steam_id;
    double due;
} simq[SIM_QUEUE];
static int nsimq;

/* Both transports pass through the same loss/latency simulation and packet decoder. */
static void received(const BYTE *buf, int len, const struct sockaddr_in *from, uint64_t steam_id, double now)
{
    if (block_until > now || len <= 0 || len > MAX_DATAGRAM)
        return;
    if (!sim_latency_ms && !sim_jitter_ms && !sim_loss_pct) {
        accept_datagram(buf, len, from, steam_id, now);
        return;
    }
    if (rand() % 100 < sim_loss_pct || nsimq == SIM_QUEUE)
        return;
    memcpy(simq[nsimq].buf, buf, len);
    simq[nsimq].len = len;
    simq[nsimq].from = *from;
    simq[nsimq].steam_id = steam_id;
    simq[nsimq].due = now + sim_latency_ms + (sim_jitter_ms ? rand() % (sim_jitter_ms + 1) : 0);
    nsimq++;
}

int net_ready(void)
{
    return net_steam ? steam_ready() : sock != INVALID_SOCKET;
}

/* A new lobby cannot inherit reliable events, snapshots or peers from the old session. */
void net_steam_session(int host, uint64_t peer)
{
    int s;

    for (s = 0; s < MAX_PLAYERS; s++)
        player_left(s, "Steam session changed");
    memset(links, 0, sizeof(links));
    memset(relay_pending, 0, sizeof(relay_pending));
    nsimq = 0;
    have_remote = 0;
    role_host = host;
    my_slot = host ? 0 : -1;
    /* A new lobby may contain a different world at the same checkpoint. */
    host_changed = !host && cur_level >= 1;
    pending_epoch = 0;
    last_save_no = 0;
    last_save_epoch = 0;
    if (host) {
        cur_epoch = (unsigned short)(nonce + next_gen++);
        slots_load();
    } else if (peer) {
        links[0].used = 1;
        links[0].steam_id = peer;
        link_reset(&links[0]);
    }
}

void net_steam_receive(const void *buf, int len, uint64_t peer)
{
    const struct sockaddr_in unused = {0};

    if (net_steam && steam_peer_allowed(peer))
        received(buf, len, &unused, peer, now_ms());
}

void net_steam_left(uint64_t peer)
{
    int s;

    for (s = 0; s < MAX_PLAYERS; s++)
        if (links[s].used && links[s].steam_id == peer) {
            player_left(s, "left Steam lobby");
            /* A client keeps its host link so a transient disconnect can reconnect. */
            if (role_host)
                links[s].used = 0;
        }
}

void net_poll(void)
{
    BYTE buf[MAX_DATAGRAM];
    struct sockaddr_in from;
    double now = now_ms();
    int n, i, fromlen;

    if (net_steam) {
        steam_poll();
    } else if (sock != INVALID_SOCKET) {
        for (i = 0; i < 256; i++) {
            fromlen = sizeof(from);
            n = recvfrom(sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
            if (n < 0 && WSAGetLastError() == WSAECONNRESET)
                continue;
            if (n <= 0)
                break;
            received(buf, n, &from, 0, now);
        }
    }
    for (i = 0; i < nsimq; ) {
        if (simq[i].due <= now) {
            if (!net_steam || steam_peer_allowed(simq[i].steam_id))
                accept_datagram(simq[i].buf, simq[i].len, &simq[i].from, simq[i].steam_id, simq[i].due);
            simq[i] = simq[--nsimq];
        } else {
            i++;
        }
    }
    check_timeouts(now);
}

/* ---- smoothness stats: how evenly a puppet moves from frame to frame ---- */

static void smooth_track(struct player *pl, int slot, const float *pos)
{
    double now = now_ms(), step = 0.0;
    int k;

    if (pl->smooth.have_last) {
        for (k = 0; k < 3; k++)
            step += (double)(pos[k] - pl->smooth.last[k]) * (pos[k] - pl->smooth.last[k]);
        step = sqrt(step);
        pl->smooth.frames++;
        pl->smooth.sum += step;
        pl->smooth.sumsq += step * step;
        if (step > pl->smooth.max)
            pl->smooth.max = step;
        if (step < 1e-4)
            pl->smooth.stalls++;
    } else {
        pl->smooth.window_start = now;
    }
    memcpy(pl->smooth.last, pos, sizeof(pl->smooth.last));
    pl->smooth.have_last = 1;

    if (now - pl->smooth.window_start >= 2000.0 && pl->smooth.frames) {
        double mean = pl->smooth.sum / pl->smooth.frames;
        double sd = sqrt(fmax(0.0, pl->smooth.sumsq / pl->smooth.frames - mean * mean));
        if (mean > 0.005)       /* only report windows where the player is moving */
            logmsg("smooth: player %d, %d frames, step mean %.4f sd %.4f max %.4f, stalled %d%%, cv %.2f", slot + 1,
                   pl->smooth.frames, mean, sd, pl->smooth.max, pl->smooth.stalls * 100 / pl->smooth.frames, sd / mean);
        memset(&pl->smooth, 0, sizeof(pl->smooth));
    }
}
