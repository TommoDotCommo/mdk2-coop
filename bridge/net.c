/*
 * Network: UDP to one peer. Player snapshots and reliable events share the
 * player packet; entity snapshots have their own packet (entities.c).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "bridge.h"

#define PLAYER_MAGIC    0x354b444d      /* "MDK5" */
#define PACKET_EVENTS   8
#define MAX_DATAGRAM    1400

struct player_packet {
    unsigned int magic;
    unsigned int seq;
    struct snap state;
    unsigned int ack;           /* highest event id received in order from the peer */
    unsigned char nevents;
    struct event events[PACKET_EVENTS];
};

/* settings from coop.ini */
int sync_anim = 1;              /* [sync] anim: 0 turns animation sync off */
int ent_hz = 15;                /* [sync] ent_hz: entity snapshots per second (host) */
static int send_hz = 30;        /* [sync] send_hz */
static int interp_ms = 100;     /* [sync] interp_ms: render the peer this far in the past; 0 = latest */
static int extrap_ms = 150;     /* [sync] extrap_ms: max extrapolation when packets are late */
static int sim_latency_ms;      /* [sim] latency_ms, jitter_ms, loss_pct: fake a bad network */
static int sim_jitter_ms;
static int sim_loss_pct;

int role_host = 1;              /* [net] role: host runs the world, client mirrors it */
int have_remote;
SOCKET sock = INVALID_SOCKET;
BYTE *local_player;             /* last gob passed to coop_send */
BYTE *puppet;                   /* last gob passed to coop_face */
struct track player_track;

static struct sockaddr_in peer;
static unsigned int send_seq, recv_seq;
static double clock_offset;     /* sender clock - local clock, least-delayed estimate */

double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

void net_open(const char *ini)
{
    char host[64], role[16];
    int listen_port, peer_port;
    struct sockaddr_in local;
    u_long nonblock = 1;
    WSADATA wsa;

    listen_port = GetPrivateProfileIntA("net", "listen_port", 27900, ini);
    peer_port = GetPrivateProfileIntA("net", "peer_port", 27901, ini);
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
    logmsg("udp listen %d, peer %s:%d, role %s", listen_port, host, peer_port, role_host ? "host" : "client");
}

void net_send(const void *buf, int len)
{
    if (sock != INVALID_SOCKET)
        sendto(sock, (const char *)buf, len, 0, (struct sockaddr *)&peer, sizeof(peer));
}

/* ---- reliable events: outbox resent until acked, inbox read in order ---- */

#define OUTBOX 256
#define INBOX  64

static struct event outbox[OUTBOX];
static unsigned int out_next = 1, out_acked;   /* outbox holds ids out_acked+1 .. out_next-1 */
static struct event inbox[INBOX];
static int inbox_head, inbox_count;
static unsigned int in_next = 1;               /* next event id expected from the peer */

struct event *event_new(int kind)
{
    struct event *e;

    if (out_next - out_acked > OUTBOX) {
        logmsg("event outbox full, dropping kind %d", kind);
        return NULL;
    }
    e = &outbox[out_next % OUTBOX];
    memset(e, 0, sizeof(*e));
    e->id = out_next++;
    e->kind = (unsigned char)kind;
    return e;
}

void event_queue(int kind, const char *name, int damage, int type, int part)
{
    struct event *e = event_new(kind);

    if (!e)
        return;
    e->damage = (short)damage;
    e->type = type;
    e->part = (signed char)part;
    snprintf(e->name, EV_NAME, "%s", name);
}

void event_queue_spawn(int kind, unsigned int netid, int objtype, const float *pos, const char *name)
{
    struct event *e = event_new(kind);

    if (!e)
        return;
    e->netid = netid;
    e->objtype = (short)objtype;
    if (pos)
        memcpy(e->pos, pos, sizeof(e->pos));
    snprintf(e->name, EV_NAME, "%s", name);
}

int event_next(struct event *out)
{
    if (!inbox_count)
        return 0;
    *out = inbox[inbox_head];
    inbox_head = (inbox_head + 1) % INBOX;
    inbox_count--;
    return 1;
}

static void events_receive(const struct player_packet *p)
{
    int i;

    if (p->ack > out_acked && p->ack < out_next)
        out_acked = p->ack;
    for (i = 0; i < p->nevents && i < PACKET_EVENTS; i++) {
        if (p->events[i].id != in_next || inbox_count == INBOX)
            continue;               /* duplicate, or a gap: wait for the resend */
        inbox[(inbox_head + inbox_count) % INBOX] = p->events[i];
        inbox_count++;
        in_next++;
    }
}

/* ---- snapshots and interpolation ---- */

static void clock_sample(unsigned int sender_time, double arrived)
{
    double o = (double)sender_time - arrived;

    if (!have_remote || o > clock_offset)
        clock_offset = o;
    else
        clock_offset -= 0.01;          /* let the estimate follow slow clock drift */
}

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

void track_update(struct track *t)
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

void net_send_player(BYTE *gob)
{
    static double next_send;
    struct player_packet p;
    double now = now_ms();
    unsigned int id;

    if (sock == INVALID_SOCKET || now < next_send)
        return;
    next_send = (now - next_send > 1000.0 / send_hz) ? now + 1000.0 / send_hz : next_send + 1000.0 / send_hz;
    memset(&p, 0, sizeof(p));
    p.magic = PLAYER_MAGIC;
    p.seq = ++send_seq;
    snap_capture(gob, &p.state);
    p.ack = in_next - 1;
    for (id = out_acked + 1; id < out_next && p.nevents < PACKET_EVENTS; id++)
        p.events[p.nevents++] = outbox[id % OUTBOX];
    net_send(&p, sizeof(p));
    if (p.seq % 300 == 0)
        logmsg("send #%u pos (%.2f, %.2f, %.2f) anim cur %d n %d, events %d unacked", p.seq,
               p.state.pos[0], p.state.pos[1], p.state.pos[2], p.state.cur, p.state.nanims, out_next - 1 - out_acked);
}

static void accept_player(const struct player_packet *p, double arrived)
{
    events_receive(p);
    if (have_remote && p->seq <= recv_seq)
        return;                         /* late or duplicate snapshot */
    clock_sample(p->state.time, arrived);
    track_add(&player_track, &p->state);
    recv_seq = p->seq;
    if (!have_remote)
        logmsg("first packet from peer (seq %u)", p->seq);
    have_remote = 1;
}

static void accept_datagram(const BYTE *buf, int len, double arrived)
{
    if (len == sizeof(struct player_packet) && *(const unsigned int *)buf == PLAYER_MAGIC) {
        const struct player_packet *p = (const struct player_packet *)buf;
        if (p->state.nanims <= SNAP_ANIMS && p->nevents <= PACKET_EVENTS)
            accept_player(p, arrived);
    } else if (have_remote) {
        ents_receive(buf, len);
    }
}

/* ---- simulated network: hold datagrams back, drop some ---- */

#define SIM_QUEUE 256

static struct { BYTE buf[MAX_DATAGRAM]; int len; double due; } simq[SIM_QUEUE];
static int nsimq;

void net_poll(void)
{
    BYTE buf[MAX_DATAGRAM];
    double now = now_ms();
    int n, i;

    if (sock == INVALID_SOCKET)
        return;
    while ((n = recv(sock, (char *)buf, sizeof(buf), 0)) > 0) {
        if (!sim_latency_ms && !sim_jitter_ms && !sim_loss_pct) {
            accept_datagram(buf, n, now);
            continue;
        }
        if (rand() % 100 < sim_loss_pct || nsimq == SIM_QUEUE)
            continue;
        memcpy(simq[nsimq].buf, buf, n);
        simq[nsimq].len = n;
        simq[nsimq].due = now + sim_latency_ms + (sim_jitter_ms ? rand() % (sim_jitter_ms + 1) : 0);
        nsimq++;
    }
    for (i = 0; i < nsimq; ) {
        if (simq[i].due <= now) {
            accept_datagram(simq[i].buf, simq[i].len, simq[i].due);
            simq[i] = simq[--nsimq];
        } else {
            i++;
        }
    }
}

/* ---- smoothness stats: how evenly the puppet moves from frame to frame ---- */

static struct {
    float last[3];
    int have_last, frames, stalls;
    double sum, sumsq, max, window_start;
} smooth;

void smooth_track(const float *pos)
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
