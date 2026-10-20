/*
 * Network: UDP, star-shaped. The host (slot 0) talks to every client; a client talks to
 * the host only. Each sends its player state at send_hz in a player packet, which also
 * carries that link's reliable events and ack. The host passes each client's state on to
 * the other clients (relayed packets: state only). Entity snapshots have their own packet
 * (entities.c), host to clients.
 *
 * Clients get their slot from the host's packets ("you"). Every process has a random
 * nonce: a new one from a known address means that game restarted, and the link starts
 * over. A player silent for PLAYER_TIMEOUT_MS has left.
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

#define PLAYER_MAGIC    0x364b444d      /* "MDK6" */
#define PACKET_EVENTS   7      /* 7 x 128-byte events + player state fit in one datagram */
#define MAX_DATAGRAM    1400
#define PLAYER_TIMEOUT_MS 10000.0
#define NO_SLOT         0xff

#define PK_STATE        1       /* carries a player state */
#define PK_RELAY        2       /* host passing on another client's state: no ack, no events */

struct player_packet {
    unsigned int magic;
    unsigned int nonce;         /* sender process */
    unsigned int seq;           /* state number of player `slot` */
    unsigned char slot;         /* whose state; NO_SLOT from a client that has no slot yet */
    unsigned char you;          /* host to client: the receiver's slot */
    unsigned char flags;
    unsigned char level;        /* level of the state; a state from another level is ignored */
    unsigned short epoch;
    short player_type;          /* OBJ type of that player (Kurt, Max, Doc, Hyde) */
    struct snap state;
    unsigned int ack;           /* highest event id received in order on this link */
    unsigned char nevents;
    struct event events[PACKET_EVENTS];
};
#define PACKET_HEAD     offsetof(struct player_packet, events)

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
int my_slot = -1;
int cur_level = -1;
int cur_cp;
unsigned short cur_epoch;
int have_remote;
SOCKET sock = INVALID_SOCKET;
BYTE *local_player;             /* last gob passed to coop_send */
BYTE *puppets[MAX_PLAYERS];

static unsigned int nonce;
static unsigned short pending_epoch;    /* client: from the last LEVEL event, taken on loading it */

/* ---- links: the host has one per client (by the client's slot), a client one to the host (0) ---- */

#define OUTBOX 256
#define INBOX  64

struct link {
    int used;
    struct sockaddr_in addr;
    unsigned int nonce;
    double last_heard;
    struct event outbox[OUTBOX];
    unsigned int out_next, out_acked;   /* outbox holds ids out_acked+1 .. out_next-1 */
    struct event inbox[INBOX];
    int inbox_head, inbox_count;
    unsigned int in_next;               /* next event id expected */
};

static struct link links[MAX_PLAYERS];

/* ---- other players, by slot ---- */

struct player {
    int connected;
    double last_heard;
    unsigned int seq;           /* latest state number */
    int type;                   /* OBJ type of their character, 0 if unknown */
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

void net_open(const char *ini)
{
    char host[64], role[16];
    int listen_port, peer_port;
    struct sockaddr_in local;
    u_long nonblock = 1;
    DWORD no_reset = FALSE, ret;
    WSADATA wsa;

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
    if (role_host)
        my_slot = 0;

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
    if (sock != INVALID_SOCKET && links[slot].used)
        sendto(sock, (const char *)buf, len, 0, (struct sockaddr *)&links[slot].addr, sizeof(links[slot].addr));
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

void event_queue_from(int to, int kind, const char *name, BYTE *from, int damage, int type, int part)
{
    struct event e;

    event_init(&e, kind);
    e.damage = (short)damage;
    e.type = type;
    e.part = (signed char)part;
    snprintf(e.name, EV_NAME, "%s", name);
    gob_peername(from, e.from, EV_NAME);
    event_send(&e, to);
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

/* Our state to every link, each with its own ack and events */
void net_send_player(BYTE *gob)
{
    static double next_send;
    static unsigned int seq;
    struct player_packet p;
    double now = now_ms();
    unsigned int id;
    int s;

    if (sock == INVALID_SOCKET || now < next_send)
        return;
    next_send = (now - next_send > 1000.0 / send_hz) ? now + 1000.0 / send_hz : next_send + 1000.0 / send_hz;
    memset(&p, 0, PACKET_HEAD);
    p.magic = PLAYER_MAGIC;
    p.nonce = nonce;
    p.seq = ++seq;
    p.slot = my_slot >= 0 ? (unsigned char)my_slot : NO_SLOT;
    p.level = (unsigned char)cur_level;
    p.epoch = cur_epoch;
    if (gob) {
        p.flags = PK_STATE;
        p.player_type = (short)gob_type(gob);
        snap_capture(gob, &p.state);
    }
    for (s = 0; s < MAX_PLAYERS; s++) {
        struct link *l = &links[s];
        if (!l->used)
            continue;
        p.you = (unsigned char)s;
        p.ack = l->in_next - 1;
        p.nevents = 0;
        for (id = l->out_acked + 1; id < l->out_next && p.nevents < PACKET_EVENTS; id++)
            p.events[p.nevents++] = l->outbox[id % OUTBOX];
        send_to(s, &p, PACKET_HEAD + p.nevents * sizeof(struct event));
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
        clock_sample(pl, p->state.time, arrived);
        track_add(&pl->track, &p->state);
    }
}

/* Host: a client's packet; its link is found (or made) by address */
static void host_accept(const struct player_packet *p, const struct sockaddr_in *from, double arrived)
{
    struct player_packet relay;
    struct link *l = NULL;
    int s, fresh = 0;

    for (s = 1; s < MAX_PLAYERS; s++)
        if (links[s].used && links[s].addr.sin_addr.s_addr == from->sin_addr.s_addr &&
            links[s].addr.sin_port == from->sin_port) {
            l = &links[s];
            break;
        }
    if (!l) {
        for (s = 1; s < MAX_PLAYERS && links[s].used; s++)
            ;
        if (s == MAX_PLAYERS) {
            static double next_log;
            if (arrived > next_log) {
                logmsg("game full: ignoring %s:%d", inet_ntoa(from->sin_addr), ntohs(from->sin_port));
                next_log = arrived + 5000.0;
            }
            return;
        }
        l = &links[s];
        memset(l, 0, sizeof(*l));
        l->used = 1;
        l->addr = *from;
        fresh = 1;
        logmsg("player %d joined from %s:%d", s + 1, inet_ntoa(from->sin_addr), ntohs(from->sin_port));
    } else if (l->nonce != p->nonce) {
        logmsg("player %d restarted its game", s + 1);
        player_left(s, "restarted");
        fresh = 1;
    }
    if (fresh) {
        link_reset(l);
        l->nonce = p->nonce;
        /* bring it into the level we are in, with every enemy there */
        if (cur_level >= 1) {
            struct event e;
            event_init(&e, EV_LEVEL);
            e.damage = (short)cur_cp;
            e.type = cur_level;
            e.netid = cur_epoch;
            event_put(l, &e);
            ents_resend(s);
            pickups_resend(s);
            logmsg("player %d asked to join level %d checkpoint %d", s + 1, cur_level, cur_cp);
        }
    }
    l->last_heard = arrived;
    events_receive(l, p);
    accept_state(s, p, arrived);
    have_remote = 1;

    /* pass the state on to the other clients */
    memcpy(&relay, p, PACKET_HEAD);
    relay.nonce = nonce;
    relay.slot = (unsigned char)s;
    relay.flags |= PK_RELAY;
    relay.ack = 0;
    relay.nevents = 0;
    for (s = 1; s < MAX_PLAYERS; s++) {
        if (!links[s].used || &links[s] == l)
            continue;
        relay.you = (unsigned char)s;
        send_to(s, &relay, PACKET_HEAD);
    }
}

/* Client: everything comes from the host */
static void client_accept(const struct player_packet *p, double arrived)
{
    struct link *l = &links[0];
    int s;

    if (l->nonce != p->nonce) {
        if (l->nonce)
            logmsg("the host restarted its game");
        for (s = 0; s < MAX_PLAYERS; s++)
            player_left(s, "host restarted");
        link_reset(l);
        l->nonce = p->nonce;
        my_slot = -1;
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
    events_receive(l, p);
    accept_state(0, p, arrived);
}

static void accept_datagram(const BYTE *buf, int len, const struct sockaddr_in *from, double arrived)
{
    const struct player_packet *p = (const struct player_packet *)buf;

    if (len >= (int)PACKET_HEAD && p->magic == PLAYER_MAGIC) {
        if (p->nevents > PACKET_EVENTS || len != (int)(PACKET_HEAD + p->nevents * sizeof(struct event)))
            return;
        if (role_host)
            host_accept(p, from, arrived);
        else
            client_accept(p, arrived);
    } else if (have_remote) {
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
 * Followed, the load takes the epoch; already there, we take it now. */
void net_level_event(const struct event *e)
{
    pending_epoch = (unsigned short)e->netid;
    if (e->type == cur_level && e->damage == cur_cp && cur_epoch != pending_epoch) {
        cur_epoch = pending_epoch;
        logmsg("already in level %d checkpoint %d: epoch %u", cur_level, cur_cp, cur_epoch);
    }
}

/* ---- simulated network: hold datagrams back, drop some ---- */

#define SIM_QUEUE 512

static struct {
    BYTE buf[MAX_DATAGRAM];
    int len;
    struct sockaddr_in from;
    double due;
} simq[SIM_QUEUE];
static int nsimq;

void net_poll(void)
{
    BYTE buf[MAX_DATAGRAM];
    struct sockaddr_in from;
    double now = now_ms();
    int n, i, fromlen;

    if (sock == INVALID_SOCKET)
        return;
    for (;;) {
        fromlen = sizeof(from);
        n = recvfrom(sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n < 0 && WSAGetLastError() == WSAECONNRESET)
            continue;
        if (n <= 0)
            break;
        if (!sim_latency_ms && !sim_jitter_ms && !sim_loss_pct) {
            accept_datagram(buf, n, &from, now);
            continue;
        }
        if (rand() % 100 < sim_loss_pct || nsimq == SIM_QUEUE)
            continue;
        memcpy(simq[nsimq].buf, buf, n);
        simq[nsimq].len = n;
        simq[nsimq].from = from;
        simq[nsimq].due = now + sim_latency_ms + (sim_jitter_ms ? rand() % (sim_jitter_ms + 1) : 0);
        nsimq++;
    }
    for (i = 0; i < nsimq; ) {
        if (simq[i].due <= now) {
            accept_datagram(simq[i].buf, simq[i].len, &simq[i].from, simq[i].due);
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
