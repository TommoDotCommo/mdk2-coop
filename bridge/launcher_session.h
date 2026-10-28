/* Portable pregame lifecycle, text chat and IMA ADPCM voice engine.
 * No Windows, sockets, UI or game dependencies. */
#ifndef LAUNCHER_SESSION_H
#define LAUNCHER_SESSION_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

enum launcher_phase { LS_OFF, LS_WAITING, LS_LOBBY, LS_COUNTDOWN, LS_LOST, LS_START };
struct launcher_session { enum launcher_phase phase; int host; uint32_t seen, deadline; };
static void ls_open(struct launcher_session *s, int host, uint32_t now)
{
    s->host = host; s->seen = now; s->deadline = 0;
    s->phase = host ? LS_LOBBY : LS_WAITING;
}
static void ls_leave(struct launcher_session *s)
{
    s->phase = LS_OFF; s->deadline = 0;
}
/* The adapter validates endpoint, roster syntax, membership and versions first. */
static int ls_roster(struct launcher_session *s, uint32_t now, int left, int valid)
{
    if (s->host || s->phase == LS_OFF || s->phase == LS_START || !valid || left < -1 || left > 3000) return 0;
    s->seen = now;
    if (left < 0) { s->phase = LS_LOBBY; s->deadline = 0; }
    else {
        /* Do not extend a countdown on each heartbeat. */
        if (s->phase != LS_COUNTDOWN) s->deadline = now + (uint32_t)left;
        s->phase = LS_COUNTDOWN;
    }
    return 1;
}
static int ls_begin(struct launcher_session *s, uint32_t now, int delay, int compatible)
{
    if (!s->host || s->phase != LS_LOBBY || !compatible || delay < 0 || delay > 3000) return 0;
    s->phase = LS_COUNTDOWN; s->deadline = now + (uint32_t)delay;
    return 1;
}
static void ls_tick(struct launcher_session *s, uint32_t now)
{
    if (!s->host && (s->phase == LS_LOBBY || s->phase == LS_COUNTDOWN) && (uint32_t)(now - s->seen) > 4000) {
        s->phase = LS_LOST; s->deadline = 0;
    }
    if (s->phase == LS_COUNTDOWN && (int32_t)(now - s->deadline) >= 0) {
        if (!s->host && (uint32_t)(now - s->seen) > 1000) {
            s->phase = LS_LOST; s->deadline = 0;
        } else s->phase = LS_START;
    }
}

/* ---- Portable pregame lobby text chat and IMA ADPCM voice chat ---- */

#define LS_MAX_MEMBERS      16
#define LS_NAME_MAX         24
#define LS_CHAT_MAX         120
#define LS_CHAT_HISTORY     16
#define LS_CHAT_WINDOW_MS   4000u
#define LS_CHAT_BURST_MAX   4
#define LS_COLORS           8

#define LS_VOICE_MAGIC      0x564b444du /* "MDKV" */
#define LS_VOICE_VERSION    1
#define LS_VOICE_RATE       16000
#define LS_VOICE_FRAME      640         /* 40 ms at 16 kHz mono */
#define LS_VOICE_BYTES      (LS_VOICE_FRAME / 2)
#define LS_VOICE_FLAG_LAST  1u
#define LS_VOICE_FLAG_VAD   2u
#define LS_VOICE_TALK_MS    250u
#define LS_VOICE_HANG_MS    250u
#define LS_VOICE_VAD_DEFAULT 1000

struct ls_chat_msg {
    uint32_t sender_id;
    uint16_t seq;
    uint8_t color;
    char name[LS_NAME_MAX];
    char text[LS_CHAT_MAX + 1];
};

struct ls_member_comm {
    uint32_t id;
    char name[LS_NAME_MAX];
    uint8_t color;
    int muted;
    int volume;                 /* 0..200 (%) */
    uint32_t chat_window_start;
    int chat_burst;
    int have_chat_seq;
    uint16_t last_chat_seq;
    int have_voice_seq;
    uint16_t last_voice_seq;
    uint32_t talking_until;
    unsigned int voice_frames;
};

struct ls_comm_state {
    int nmembers;
    struct ls_member_comm members[LS_MAX_MEMBERS];
    struct ls_chat_msg history[LS_CHAT_HISTORY];
    int history_total;          /* total accepted messages; newest at (history_total - 1) % LS_CHAT_HISTORY */
    uint16_t next_chat_seq;
    uint16_t next_voice_seq;
    int enc_pred, enc_index;
    uint32_t local_hang_until;
    int local_was_sending;
    uint32_t local_talking_until;
};

struct ls_voice_pkt {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t seq;
    uint32_t sender_id;
    int16_t pred;
    uint8_t index;
    uint8_t pad;
    uint8_t data[LS_VOICE_BYTES];
};

/* Sanitise untrusted text for UI/wire: strip control characters (<32 or >=127) and CR/LF,
 * replace '|' delimiter with '_', trim leading/trailing spaces, cap length. */
static inline size_t ls_sanitize_text(const char *in, char *out, size_t cap, int replace_pipe)
{
    size_t i = 0, n = 0;
    if (!in || !out || cap < 2) { if (out && cap) out[0] = 0; return 0; }
    while (in[i] == ' ' || in[i] == '\t') i++;
    for (; in[i] && n + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 32 || c >= 127) continue;
        if (replace_pipe && c == '|') c = '_';
        out[n++] = (char)c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
    return n;
}

static inline void ls_comm_init(struct ls_comm_state *c)
{
    memset(c, 0, sizeof(*c));
    c->next_chat_seq = 1;
    c->next_voice_seq = 1;
}

/* Synchronize the communication roster with the active lobby member list, preserving per-member
 * mute, volume, rate-limit and sequence state across roster refreshes. */
static inline void ls_comm_sync_roster(struct ls_comm_state *c, int count, const uint32_t *ids,
                                       const char names[][LS_NAME_MAX])
{
    struct ls_member_comm next_m[LS_MAX_MEMBERS];
    int i, j;
    if (count < 0) count = 0;
    if (count > LS_MAX_MEMBERS) count = LS_MAX_MEMBERS;
    memset(next_m, 0, sizeof(next_m));
    for (i = 0; i < count; i++) {
        next_m[i].id = ids[i];
        ls_sanitize_text(names[i], next_m[i].name, sizeof(next_m[i].name), 1);
        if (!next_m[i].name[0]) strcpy(next_m[i].name, "Player");
        next_m[i].color = (uint8_t)(ids[i] % LS_COLORS);
        next_m[i].volume = 100;
        for (j = 0; j < c->nmembers; j++) {
            if (c->members[j].id == ids[i]) {
                next_m[i].color = c->members[j].color;
                next_m[i].muted = c->members[j].muted;
                next_m[i].volume = c->members[j].volume;
                next_m[i].chat_window_start = c->members[j].chat_window_start;
                next_m[i].chat_burst = c->members[j].chat_burst;
                next_m[i].have_chat_seq = c->members[j].have_chat_seq;
                next_m[i].last_chat_seq = c->members[j].last_chat_seq;
                next_m[i].have_voice_seq = c->members[j].have_voice_seq;
                next_m[i].last_voice_seq = c->members[j].last_voice_seq;
                next_m[i].talking_until = c->members[j].talking_until;
                next_m[i].voice_frames = c->members[j].voice_frames;
                break;
            }
        }
    }
    c->nmembers = count;
    memcpy(c->members, next_m, sizeof(next_m));
}

static inline struct ls_member_comm *ls_comm_find(struct ls_comm_state *c, uint32_t id)
{
    int i;
    for (i = 0; i < c->nmembers; i++)
        if (c->members[i].id == id) return &c->members[i];
    return NULL;
}

static inline void ls_chat_append(struct ls_comm_state *c, uint32_t sender_id, uint16_t seq,
                                  uint8_t color, const char *name, const char *text)
{
    struct ls_chat_msg *slot = &c->history[c->history_total % LS_CHAT_HISTORY];
    slot->sender_id = sender_id;
    slot->seq = seq;
    slot->color = (uint8_t)(color % LS_COLORS);
    ls_sanitize_text(name, slot->name, sizeof(slot->name), 1);
    if (!slot->name[0]) strcpy(slot->name, "Player");
    ls_sanitize_text(text, slot->text, sizeof(slot->text), 0);
    c->history_total++;
}

static inline int ls_chat_check_flood_and_seq(struct ls_member_comm *m, uint32_t now, uint16_t seq)
{
    if (m->have_chat_seq && seq == m->last_chat_seq) return 0;
    if (!m->chat_burst || (uint32_t)(now - m->chat_window_start) >= LS_CHAT_WINDOW_MS) {
        m->chat_window_start = now;
        m->chat_burst = 0;
    }
    if (m->chat_burst >= LS_CHAT_BURST_MAX) return 0;
    m->chat_burst++;
    m->have_chat_seq = 1;
    m->last_chat_seq = seq;
    return 1;
}

/* Format client -> host chat packet: "MDKL C1 <id_hex> <seq> <color>|<text>" */
static inline int ls_chat_encode_c1(uint32_t sender_id, uint16_t seq, uint8_t color,
                                    const char *raw_text, char *out, size_t cap)
{
    char clean[LS_CHAT_MAX + 1];
    int n;
    if (!ls_sanitize_text(raw_text, clean, sizeof(clean), 1)) return 0;
    n = snprintf(out, cap, "MDKL C1 %x %u %u|%s", sender_id, (unsigned)seq, (unsigned)(color % LS_COLORS), clean);
    return (n > 0 && (size_t)n < cap) ? n : 0;
}

/* Format host -> members chat broadcast: "MDKL M1 <id_hex> <seq> <color>|<name>|<text>" */
static inline int ls_chat_encode_m1(uint32_t sender_id, uint16_t seq, uint8_t color,
                                    const char *raw_name, const char *raw_text, char *out, size_t cap)
{
    char clean_name[LS_NAME_MAX], clean_text[LS_CHAT_MAX + 1];
    int n;
    if (!ls_sanitize_text(raw_text, clean_text, sizeof(clean_text), 1)) return 0;
    if (!ls_sanitize_text(raw_name, clean_name, sizeof(clean_name), 1)) strcpy(clean_name, "Player");
    n = snprintf(out, cap, "MDKL M1 %x %u %u|%s|%s", sender_id, (unsigned)seq, (unsigned)(color % LS_COLORS), clean_name, clean_text);
    return (n > 0 && (size_t)n < cap) ? n : 0;
}

/* Host receives C1 from a client, validates membership, endpoint, flood limit & sanitisation,
 * appends to host history and formats M1 broadcast for all joined lobby members. */
static inline int ls_chat_host_recv_c1(const struct launcher_session *s, struct ls_comm_state *c,
                                       uint32_t now, const char *pkt, size_t len, int endpoint_ok,
                                       char *m1_out, size_t m1_cap)
{
    unsigned id = 0, seq = 0, color = 0;
    const char *bar;
    char clean[LS_CHAT_MAX + 1];
    struct ls_member_comm *m;
    if (!s->host || (s->phase != LS_LOBBY && s->phase != LS_COUNTDOWN) || !endpoint_ok) return 0;
    if (!pkt || len < 14 || len > 512 || memchr(pkt, 0, len) || strncmp(pkt, "MDKL C1 ", 8)) return 0;
    bar = (const char *)memchr(pkt + 8, '|', len - 8);
    if (!bar || (size_t)(bar - pkt) > 48) return 0;
    {
        char hdr[56];
        size_t hlen = (size_t)(bar - (pkt + 8));
        int consumed = 0;
        memcpy(hdr, pkt + 8, hlen); hdr[hlen] = 0;
        if (sscanf(hdr, "%x %u %u%n", &id, &seq, &color, &consumed) < 3 || (size_t)consumed != hlen) return 0;
    }
    if (seq > 65535u || color >= LS_COLORS) return 0;
    if (len - (size_t)(bar + 1 - pkt) > LS_CHAT_MAX) return 0;
    {
        char raw_body[LS_CHAT_MAX + 1];
        size_t blen = len - (size_t)(bar + 1 - pkt);
        memcpy(raw_body, bar + 1, blen); raw_body[blen] = 0;
        if (!ls_sanitize_text(raw_body, clean, sizeof(clean), 1)) return 0;
    }
    m = ls_comm_find(c, id);
    if (!m || m == &c->members[0]) return 0; /* C1 comes only from joined clients 1..n-1 */
    if (!ls_chat_check_flood_and_seq(m, now, (uint16_t)seq)) return 0;
    m->color = (uint8_t)color;
    ls_chat_append(c, id, (uint16_t)seq, (uint8_t)color, m->name, clean);
    return ls_chat_encode_m1(id, (uint16_t)seq, (uint8_t)color, m->name, clean, m1_out, m1_cap);
}

/* Client receives M1 from the configured host endpoint. */
static inline int ls_chat_client_recv_m1(const struct launcher_session *s, struct ls_comm_state *c,
                                         uint32_t now, const char *pkt, size_t len, int endpoint_ok)
{
    unsigned id = 0, seq = 0, color = 0;
    const char *bar1, *bar2;
    char raw_name[64], raw_text[LS_CHAT_MAX + 1], clean_name[LS_NAME_MAX], clean_text[LS_CHAT_MAX + 1];
    struct ls_member_comm *m;
    size_t nlen, tlen;
    if (s->host || (s->phase != LS_LOBBY && s->phase != LS_COUNTDOWN) || !endpoint_ok) return 0;
    if (!pkt || len < 16 || len > 512 || memchr(pkt, 0, len) || strncmp(pkt, "MDKL M1 ", 8)) return 0;
    bar1 = (const char *)memchr(pkt + 8, '|', len - 8);
    if (!bar1 || (size_t)(bar1 - pkt) > 48) return 0;
    bar2 = (const char *)memchr(bar1 + 1, '|', len - (size_t)(bar1 + 1 - pkt));
    if (!bar2) return 0;
    {
        char hdr[56];
        size_t hlen = (size_t)(bar1 - (pkt + 8));
        int consumed = 0;
        memcpy(hdr, pkt + 8, hlen); hdr[hlen] = 0;
        if (sscanf(hdr, "%x %u %u%n", &id, &seq, &color, &consumed) < 3 || (size_t)consumed != hlen) return 0;
    }
    if (seq > 65535u || color >= LS_COLORS) return 0;
    nlen = (size_t)(bar2 - (bar1 + 1));
    tlen = len - (size_t)(bar2 + 1 - pkt);
    if (!nlen || nlen >= sizeof(raw_name) || !tlen || tlen > LS_CHAT_MAX) return 0;
    memcpy(raw_name, bar1 + 1, nlen); raw_name[nlen] = 0;
    memcpy(raw_text, bar2 + 1, tlen); raw_text[tlen] = 0;
    if (!ls_sanitize_text(raw_name, clean_name, sizeof(clean_name), 1) ||
        !ls_sanitize_text(raw_text, clean_text, sizeof(clean_text), 1)) return 0;
    m = ls_comm_find(c, id);
    if (!m) return 0;
    if (!ls_chat_check_flood_and_seq(m, now, (uint16_t)seq)) return 0;
    m->color = (uint8_t)color;
    ls_chat_append(c, id, (uint16_t)seq, (uint8_t)color, clean_name, clean_text);
    return 1;
}

/* ---- IMA ADPCM codec & VAD (shared with bridge/voice.c) ---- */

static inline int ls_adpcm_step(int code, int *pred, int *index)
{
    static const short step_table[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
        4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
        22385, 24623, 27086, 29794, 32767
    };
    static const signed char index_table[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
    int step = step_table[*index], diff = step >> 3;
    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    *pred += (code & 8) ? -diff : diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *index += index_table[code & 15];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return *pred;
}

static inline void ls_adpcm_encode(const int16_t *pcm, struct ls_voice_pkt *p, int *pred, int *index)
{
    static const short step_table[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
        4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
        22385, 24623, 27086, 29794, 32767
    };
    int i;
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    p->pred = (int16_t)*pred;
    p->index = (uint8_t)*index;
    for (i = 0; i < LS_VOICE_FRAME; i++) {
        int step = step_table[*index], diff = pcm[i] - *pred, code = 0;
        if (diff < 0) { code = 8; diff = -diff; }
        if (diff >= step) { code |= 4; diff -= step; }
        if (diff >= (step >> 1)) { code |= 2; diff -= step >> 1; }
        if (diff >= (step >> 2)) code |= 1;
        ls_adpcm_step(code, pred, index);
        if (i & 1) p->data[i / 2] |= (uint8_t)(code << 4);
        else p->data[i / 2] = (uint8_t)code;
    }
}

static inline void ls_adpcm_decode(const struct ls_voice_pkt *p, int16_t *pcm)
{
    int pred = p->pred, index = p->index > 88 ? 88 : p->index, i;
    for (i = 0; i < LS_VOICE_FRAME; i++)
        pcm[i] = (int16_t)ls_adpcm_step((p->data[i / 2] >> ((i & 1) * 4)) & 15, &pred, &index);
}

static inline int ls_pcm_level(const int16_t *pcm, size_t n)
{
    uint64_t sum = 0;
    size_t i;
    if (!pcm || !n) return 0;
    for (i = 0; i < n; i++) {
        int v = pcm[i];
        sum += (uint64_t)(v < 0 ? -v : v);
    }
    return (int)(sum / n);
}

static inline int ls_voice_should_send(struct ls_comm_state *c, int mode_vad, int ptt_down,
                                       const int16_t *pcm, int vad_threshold, uint32_t now, uint8_t *out_flags)
{
    int active = mode_vad ? (ls_pcm_level(pcm, LS_VOICE_FRAME) >= vad_threshold) : (ptt_down != 0);
    *out_flags = mode_vad ? LS_VOICE_FLAG_VAD : 0;
    if (active) {
        c->local_hang_until = now + LS_VOICE_HANG_MS;
        c->local_was_sending = 1;
        c->local_talking_until = now + LS_VOICE_TALK_MS;
        return 1;
    }
    if (c->local_was_sending && (int32_t)(c->local_hang_until - now) > 0) {
        c->local_talking_until = now + LS_VOICE_TALK_MS;
        return 1;
    }
    if (c->local_was_sending) {
        c->local_was_sending = 0;
        c->local_talking_until = 0;
        *out_flags |= LS_VOICE_FLAG_LAST;
        return 1;
    }
    return 0;
}

static inline void ls_voice_pack(struct ls_comm_state *c, uint32_t sender_id, uint8_t flags,
                                 const int16_t *pcm, struct ls_voice_pkt *out)
{
    int16_t zero[LS_VOICE_FRAME];
    memset(out, 0, sizeof(*out));
    out->magic = LS_VOICE_MAGIC;
    out->version = LS_VOICE_VERSION;
    out->flags = flags;
    out->seq = c->next_voice_seq++;
    out->sender_id = sender_id;
    if (flags & LS_VOICE_FLAG_LAST) {
        memset(zero, 0, sizeof(zero));
        pcm = zero;
    }
    ls_adpcm_encode(pcm, out, &c->enc_pred, &c->enc_index);
}

static inline int ls_voice_validate(const void *buf, size_t len, struct ls_voice_pkt *out)
{
    if (!buf || len != sizeof(struct ls_voice_pkt)) return 0;
    memcpy(out, buf, sizeof(*out));
    if (out->magic != LS_VOICE_MAGIC || out->version != LS_VOICE_VERSION) return 0;
    if (out->index > 88 || (out->flags & ~(LS_VOICE_FLAG_LAST | LS_VOICE_FLAG_VAD)) != 0 || out->pad != 0) return 0;
    return 1;
}

/* Validate and process an incoming pregame voice frame.
 * Enforces member-only routing, endpoint check, sequence deduplication, per-player mute/volume
 * and speaking indicators. Nothing is ever written to disk. */
static inline int ls_voice_accept(const struct launcher_session *s, struct ls_comm_state *c,
                                  uint32_t now, const void *buf, size_t len, int endpoint_ok,
                                  uint32_t self_id, int16_t *pcm_out, int *out_played,
                                  uint32_t *relay_ids, int *out_relay_count)
{
    struct ls_voice_pkt pkt;
    struct ls_member_comm *m;
    int i, rcount = 0;
    *out_played = 0;
    if (out_relay_count) *out_relay_count = 0;
    if ((s->phase != LS_LOBBY && s->phase != LS_COUNTDOWN) || !endpoint_ok) return 0;
    if (!ls_voice_validate(buf, len, &pkt)) return 0;
    if (pkt.sender_id == self_id) return 0;
    m = ls_comm_find(c, pkt.sender_id);
    if (!m || (s->host && m == &c->members[0])) return 0;
    if (m->have_voice_seq) {
        uint16_t delta = (uint16_t)(pkt.seq - m->last_voice_seq);
        if (delta == 0 || delta >= 1000u) return 0;
    }
    m->have_voice_seq = 1;
    m->last_voice_seq = pkt.seq;
    m->voice_frames++;
    if (pkt.flags & LS_VOICE_FLAG_LAST) m->talking_until = 0;
    else m->talking_until = now + LS_VOICE_TALK_MS;

    if (s->host && relay_ids && out_relay_count) {
        for (i = 1; i < c->nmembers; i++) {
            if (c->members[i].id != pkt.sender_id)
                relay_ids[rcount++] = c->members[i].id;
        }
        *out_relay_count = rcount;
    }

    if (m->muted || m->volume <= 0) {
        if (pcm_out) memset(pcm_out, 0, LS_VOICE_FRAME * sizeof(int16_t));
        return 1;
    }
    if (pcm_out) {
        ls_adpcm_decode(&pkt, pcm_out);
        if (m->volume != 100) {
            int vol = m->volume > 200 ? 200 : m->volume;
            for (i = 0; i < LS_VOICE_FRAME; i++) {
                int v = ((int)pcm_out[i] * vol) / 100;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                pcm_out[i] = (int16_t)v;
            }
        }
        *out_played = 1;
    }
    return 1;
}

static inline int ls_member_talking(const struct ls_member_comm *m, uint32_t now)
{
    return m && m->talking_until && (int32_t)(m->talking_until - now) > 0;
}

static inline int ls_local_talking(const struct ls_comm_state *c, uint32_t now)
{
    return c && c->local_talking_until && (int32_t)(c->local_talking_until - now) > 0;
}

static inline uint32_t ls_color_rgb(uint8_t color_idx)
{
    static const uint32_t palette[LS_COLORS] = {
        0x45c9ff, /* cyan/blue */
        0xff8f1f, /* orange */
        0x5ae085, /* green */
        0xffd34d, /* yellow */
        0xd178ff, /* purple/magenta */
        0xff5a4a, /* coral red */
        0x5ee3d1, /* teal */
        0xe9f0f2, /* silver/white */
    };
    return palette[color_idx % LS_COLORS];
}

#endif
