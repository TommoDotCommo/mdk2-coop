/*
 * Proximity voice chat: hold the push-to-talk key and nearby players hear you, quieter the
 * further away you are.
 *
 * Mic and speakers go through winmm on a worker thread: 16 kHz mono, 40 ms frames, IMA ADPCM
 * (4 bit, about 66 kbit/s a talker; no codec library). Each frame is one datagram with its own
 * predictor state, so a lost frame costs 40 ms and nothing after it. Same star as the rest of
 * the net: a client sends to the host, the host plays it and passes it on only to the players
 * who are in range of the speaker (so far-away talk costs no bandwidth), stamping the speaker's
 * slot and the host's range. The range is the host's setting, like friendly fire; each listener
 * turns the speaker's distance into a volume (full close up, fading to silence at the range).
 *
 *   [voice]  enabled=1|0, key=V|B|T|CAPS (push to talk), volume=<% of normal>, range=<world units, 0 = everywhere>
 *
 * Network, key and gains are handled on the game thread (voice_frame); the worker only touches the
 * mic, the jitter buffers (under a lock) and the speakers. Background test mode opens no audio
 * device: coop_voice_test(ms) makes a tone instead of mic input and the speakers are only
 * simulated, so tests can count frames and gains without a sound.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "bridge.h"
#include <mmsystem.h>

#define VOICE_MAGIC     0x434f564d      /* "MVOC" */
#define RATE            16000
#define FRAME           640             /* samples per datagram: 40 ms */
#define VOICE_BYTES     (FRAME / 2)
#define CHUNK           320             /* output buffer: 20 ms */
#define NBUF            4
#define RING            8192            /* samples of jitter buffer per speaker */
#define PRIME           (FRAME * 2)     /* start playing with 80 ms buffered */
#define MAX_BUFFERED    (FRAME * 8)
#define HANG_MS         250.0           /* keep sending this long after the key is released */
#define TALK_MS         250.0           /* "is talking" for this long after the last frame */
#define TXQ             16
#define FLAG_LAST       1

struct voice_packet {
    unsigned int magic;
    unsigned char slot;         /* speaker; a client leaves it, the host sets it when passing on */
    unsigned char flags;
    unsigned short seq;
    unsigned short range;       /* the host's range, world units, 0 = everywhere */
    short pred;                 /* ADPCM state at the start of the frame */
    unsigned char index, pad;
    unsigned char data[VOICE_BYTES];
};
#define VOICE_HEAD      offsetof(struct voice_packet, data)

/* settings */
static int cfg_enabled = 1;
static int cfg_dik = 0x2f, cfg_vk = 'V';
static int cfg_volume = 100;
static int cfg_range = 50;

static CRITICAL_SECTION lock;
static int started;

/* ---- IMA ADPCM ---- */

static const short step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767
};
static const signed char index_table[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int adpcm_step(int code, int *pred, int *index)
{
    int step = step_table[*index], diff = step >> 3;

    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    *pred += (code & 8) ? -diff : diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *index += index_table[code];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return *pred;
}

static void adpcm_encode(const short *pcm, struct voice_packet *p, int *pred, int *index)
{
    int i;

    p->pred = (short)*pred;
    p->index = (unsigned char)*index;
    for (i = 0; i < FRAME; i++) {
        int step = step_table[*index], diff = pcm[i] - *pred, code = 0;

        if (diff < 0) { code = 8; diff = -diff; }
        if (diff >= step) { code |= 4; diff -= step; }
        if (diff >= step >> 1) { code |= 2; diff -= step >> 1; }
        if (diff >= step >> 2) code |= 1;
        adpcm_step(code, pred, index);
        if (i & 1) p->data[i / 2] |= code << 4; else p->data[i / 2] = (unsigned char)code;
    }
}

static void adpcm_decode(const struct voice_packet *p, short *pcm)
{
    int pred = p->pred, index = p->index > 88 ? 88 : p->index, i;

    for (i = 0; i < FRAME; i++)
        pcm[i] = (short)adpcm_step((p->data[i / 2] >> ((i & 1) * 4)) & 15, &pred, &index);
}

/* ---- speakers (other players), by slot ---- */

struct speaker {
    short ring[RING];
    unsigned int rd, wr;        /* sample counters; buffered = wr - rd */
    int primed;
    float gain, target;         /* gain applied to the last chunk, and wanted (game thread sets target) */
    unsigned short seq;
    int have_seq;
    double last_rx;
    unsigned int frames, lost;
    int range;
};

static struct speaker spk[MAX_PLAYERS];

/* ---- our side: a queue of encoded frames from the worker to the game thread ---- */

static struct voice_packet txq[TXQ];
static int tx_head, tx_count;
static unsigned short tx_seq;
static int enc_pred, enc_index;
static volatile int ptt;                /* the game thread: key held (or the test tone) */
static double tx_last;                  /* last frame we sent */
static unsigned int tx_frames;
static double tone_until;

static void tx_push(const short *pcm, int last)
{
    struct voice_packet p;

    memset(&p, 0, sizeof(p));
    p.magic = VOICE_MAGIC;
    p.flags = last ? FLAG_LAST : 0;
    adpcm_encode(pcm, &p, &enc_pred, &enc_index);
    EnterCriticalSection(&lock);
    if (tx_count < TXQ) {
        p.seq = tx_seq++;
        txq[(tx_head + tx_count++) % TXQ] = p;
    }
    LeaveCriticalSection(&lock);
}

/* ---- worker thread: mic in, speakers out ---- */

static HANDLE in_ev, out_ev;
static HWAVEIN hin;
static HWAVEOUT hout;
static WAVEHDR in_hdr[NBUF], out_hdr[NBUF];
static short in_buf[NBUF][FRAME], out_buf[NBUF][CHUNK];
static int in_open, in_failed, out_open, out_failed, out_wanted;
static volatile int quit;

static void wave_format(WAVEFORMATEX *f)
{
    memset(f, 0, sizeof(*f));
    f->wFormatTag = WAVE_FORMAT_PCM;
    f->nChannels = 1;
    f->nSamplesPerSec = RATE;
    f->wBitsPerSample = 16;
    f->nBlockAlign = 2;
    f->nAvgBytesPerSec = RATE * 2;
}

static void mic_open(void)
{
    WAVEFORMATEX f;
    int i;
    MMRESULT r;

    wave_format(&f);
    if ((r = waveInOpen(&hin, WAVE_MAPPER, &f, (DWORD_PTR)in_ev, 0, CALLBACK_EVENT)) != MMSYSERR_NOERROR) {
        logmsg("voice: no microphone (waveInOpen %d)", (int)r);
        in_failed = 1;
        return;
    }
    for (i = 0; i < NBUF; i++) {
        memset(&in_hdr[i], 0, sizeof(in_hdr[i]));
        in_hdr[i].lpData = (LPSTR)in_buf[i];
        in_hdr[i].dwBufferLength = sizeof(in_buf[i]);
        waveInPrepareHeader(hin, &in_hdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(hin, &in_hdr[i], sizeof(WAVEHDR));
    }
    waveInStart(hin);
    in_open = 1;
    logmsg("voice: microphone open");
}

static void speakers_open(void)
{
    WAVEFORMATEX f;
    int i;
    MMRESULT r;

    wave_format(&f);
    if ((r = waveOutOpen(&hout, WAVE_MAPPER, &f, (DWORD_PTR)out_ev, 0, CALLBACK_EVENT)) != MMSYSERR_NOERROR) {
        logmsg("voice: no speakers (waveOutOpen %d)", (int)r);
        out_failed = 1;
        return;
    }
    for (i = 0; i < NBUF; i++) {
        memset(&out_hdr[i], 0, sizeof(out_hdr[i]));
        out_hdr[i].lpData = (LPSTR)out_buf[i];
        out_hdr[i].dwBufferLength = sizeof(out_buf[i]);
        waveOutPrepareHeader(hout, &out_hdr[i], sizeof(WAVEHDR));
        out_hdr[i].dwFlags |= WHDR_DONE;        /* free: filled by the first service pass */
    }
    out_open = 1;
    logmsg("voice: speakers open");
}

/* mix the next CHUNK samples of every speaker */
static void mix(short *out)
{
    int acc[CHUNK], s, i;

    memset(acc, 0, sizeof(acc));
    EnterCriticalSection(&lock);
    for (s = 0; s < MAX_PLAYERS; s++) {
        struct speaker *k = &spk[s];
        unsigned int buffered = k->wr - k->rd;
        float g, step;

        if (!k->primed) {
            if (buffered >= PRIME)
                k->primed = 1;
            else
                continue;
        }
        if (buffered < CHUNK) {
            k->primed = 0;
            continue;
        }
        g = k->gain;
        step = (k->target - k->gain) / CHUNK;
        for (i = 0; i < CHUNK; i++, g += step)
            acc[i] += (int)(k->ring[(k->rd + i) % RING] * g);
        k->rd += CHUNK;
        k->gain = k->target;
    }
    LeaveCriticalSection(&lock);
    for (i = 0; i < CHUNK; i++)
        out[i] = (short)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
}

static void capture_service(void)
{
    static int collected;
    static short frame[FRAME];
    static int hang;
    int i;

    if (test_background) {
        static double next;
        double now = now_ms();
        if (now < next)
            return;
        next = now + 1000.0 * FRAME / RATE;
        if (now < tone_until) {
            for (i = 0; i < FRAME; i++)
                frame[i] = (short)(8000 * sin(2 * 3.14159265 * 440 * i / RATE));
            tx_push(frame, 0);
            hang = 1;
        } else if (hang) {
            hang = 0;
            memset(frame, 0, sizeof(frame));
            tx_push(frame, 1);
        }
        return;
    }
    if (!in_open && !in_failed && ptt)
        mic_open();
    if (!in_open)
        return;
    for (i = 0; i < NBUF; i++) {
        WAVEHDR *h = &in_hdr[i];
        if (!(h->dwFlags & WHDR_DONE))
            continue;
        if (h->dwBytesRecorded == sizeof(in_buf[i])) {
            static double talk_until;
            double now = now_ms();
            if (ptt) {
                talk_until = now + HANG_MS;
                tx_push(in_buf[i], 0);
                collected = 1;
            } else if (collected && now < talk_until) {
                tx_push(in_buf[i], 0);
            } else if (collected) {
                collected = 0;
                memset(frame, 0, sizeof(frame));
                tx_push(frame, 1);
            }
        }
        waveInAddBuffer(hin, h, sizeof(WAVEHDR));
    }
}

static void playback_service(void)
{
    int i;

    if (test_background) {
        static double next;
        double now = now_ms();
        short scratch[CHUNK];
        while (next < now) {
            if (next == 0.0)
                next = now;
            mix(scratch);
            next += 1000.0 * CHUNK / RATE;
        }
        return;
    }
    if (!out_open && !out_failed && out_wanted)
        speakers_open();
    if (!out_open)
        return;
    for (i = 0; i < NBUF; i++) {
        WAVEHDR *h = &out_hdr[i];
        if (!(h->dwFlags & WHDR_DONE))
            continue;
        mix(out_buf[i]);
        h->dwFlags &= ~WHDR_DONE;
        waveOutWrite(hout, h, sizeof(WAVEHDR));
    }
}

static DWORD WINAPI worker(LPVOID arg)
{
    HANDLE ev[2];

    ev[0] = in_ev;
    ev[1] = out_ev;
    while (!quit) {
        WaitForMultipleObjects(2, ev, FALSE, 10);
        capture_service();
        playback_service();
    }
    return 0;
}

/* ---- settings ---- */

static void key_from_name(const char *name)
{
    if (!_stricmp(name, "B")) { cfg_dik = 0x30; cfg_vk = 'B'; }
    else if (!_stricmp(name, "T")) { cfg_dik = 0x14; cfg_vk = 'T'; }
    else if (!_stricmp(name, "CAPS")) { cfg_dik = 0x3a; cfg_vk = VK_CAPITAL; }
    else { cfg_dik = 0x2f; cfg_vk = 'V'; }
}

void voice_reload(const char *ini)
{
    char key[16];

    cfg_enabled = GetPrivateProfileIntA("voice", "enabled", 1, ini) != 0;
    cfg_volume = GetPrivateProfileIntA("voice", "volume", 100, ini);
    if (cfg_volume < 0) cfg_volume = 0;
    if (cfg_volume > 300) cfg_volume = 300;
    cfg_range = GetPrivateProfileIntA("voice", "range", 50, ini);
    if (cfg_range < 0) cfg_range = 0;
    if (cfg_range > 60000) cfg_range = 60000;
    GetPrivateProfileStringA("voice", "key", "V", key, sizeof(key), ini);
    key_from_name(key);
}

void voice_init(const char *ini)
{
    voice_reload(ini);
    InitializeCriticalSection(&lock);
    in_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    out_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    started = CreateThread(NULL, 0, worker, NULL, 0, NULL) != NULL;
    logmsg("voice: %s, push to talk %c, range %d, volume %d%%%s", cfg_enabled ? "on" : "off",
           cfg_vk == VK_CAPITAL ? '*' : cfg_vk, cfg_range, cfg_volume, test_background ? " (no audio devices: test mode)" : "");
}

/* ---- network ---- */

int voice_is_packet(const void *buf, int len)
{
    return len >= (int)sizeof(unsigned int) && *(const unsigned int *)buf == VOICE_MAGIC;
}

static float dist_between(BYTE *a, BYTE *b)
{
    const float *p = (const float *)(a + GOB_POSITION), *q = (const float *)(b + GOB_POSITION);
    float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];

    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* the gob of a player in our world: our own character for our slot */
static BYTE *gob_of(int slot)
{
    BYTE *g = slot == my_slot ? local_player : puppets[slot];

    return gob_alive(g) ? g : NULL;
}

/* host: pass a frame from `speaker` on to every other client that is in range of them */
static void forward(int speaker, struct voice_packet *p)
{
    int t;
    BYTE *from = gob_of(speaker);

    p->slot = (unsigned char)speaker;
    p->range = (unsigned short)cfg_range;
    for (t = 1; t < MAX_PLAYERS; t++) {
        BYTE *to;

        if (t == speaker || !player_in_level(t))
            continue;
        to = gob_of(t);
        if (cfg_range && from && to && dist_between(from, to) > cfg_range * 1.1f)
            continue;
        net_send_to(t, p, sizeof(*p));
    }
}

/* net.c: a voice datagram from link `from` (the speaker's slot on the host, 0 on a client) */
void voice_receive(int from, const void *buf, int len)
{
    struct voice_packet p;
    short pcm[FRAME];
    struct speaker *k;
    int slot;

    if (len != (int)sizeof(p) || !cfg_enabled)
        return;
    memcpy(&p, buf, sizeof(p));
    slot = role_host ? from : p.slot;
    if (slot < 0 || slot >= MAX_PLAYERS || slot == my_slot || !player_connected(slot))
        return;
    k = &spk[slot];
    adpcm_decode(&p, pcm);
    EnterCriticalSection(&lock);
    if (k->have_seq && (unsigned short)(p.seq - k->seq) != 0 && (unsigned short)(p.seq - k->seq) < 1000)
        k->lost += (unsigned short)(p.seq - k->seq) - 1;
    if (!(k->have_seq && (unsigned short)(p.seq - k->seq) >= 1000)) {      /* not an old straggler */
        k->seq = p.seq;
        k->have_seq = 1;
        if (k->wr - k->rd > MAX_BUFFERED)
            k->rd = k->wr - PRIME;
        {
            int i;
            for (i = 0; i < FRAME; i++)
                k->ring[(k->wr + i) % RING] = pcm[i];
        }
        k->wr += FRAME;
        k->range = role_host ? cfg_range : p.range;
        k->frames++;
        if (p.flags & FLAG_LAST)
            k->last_rx = 0;
        else
            k->last_rx = now_ms();
    }
    LeaveCriticalSection(&lock);
    out_wanted = 1;
    if (role_host)
        forward(slot, &p);
}

/* volume of a speaker `slot` for us: full close up, fading to nothing at the speaker's range */
static float gain_for(int slot)
{
    BYTE *a, *b;
    float near_d, d, t, g;
    int range = spk[slot].range;

    if (!player_in_level(slot))
        return 0.0f;
    if (!range)
        g = 1.0f;
    else if (!(a = gob_of(slot)) || !(b = gob_of(my_slot)))
        g = 0.5f;
    else {
        d = dist_between(a, b);
        near_d = range * 0.2f;
        t = d <= near_d ? 0.0f : (d - near_d) / (range - near_d);
        g = t >= 1.0f ? 0.0f : (1.0f - t) * (1.0f - t);
    }
    return g * cfg_volume / 100.0f;
}

/* once per frame, game thread: key, gains, and frames the worker has queued */
void voice_frame(void)
{
    static int was_talking;
    double now = now_ms();
    int s, down;

    if (!started)
        return;
    down = cfg_enabled && (input_down(cfg_dik, cfg_vk) || now < tone_until);
    ptt = down && have_remote;
    for (s = 0; s < MAX_PLAYERS; s++)
        spk[s].target = gain_for(s);
    for (;;) {
        struct voice_packet p;

        EnterCriticalSection(&lock);
        if (!tx_count) {
            LeaveCriticalSection(&lock);
            break;
        }
        p = txq[tx_head];
        tx_head = (tx_head + 1) % TXQ;
        tx_count--;
        LeaveCriticalSection(&lock);
        if (!have_remote || !net_ready())
            continue;
        tx_frames++;
        tx_last = now;
        p.slot = (unsigned char)(my_slot < 0 ? 0xff : my_slot);
        p.range = (unsigned short)cfg_range;
        if (role_host)
            forward(my_slot < 0 ? 0 : my_slot, &p);
        else
            net_send_to(0, &p, sizeof(p));
    }
    if ((now - tx_last < TALK_MS) != was_talking) {
        was_talking = !was_talking;
        logmsg("voice: %s talking (%u frames sent)", was_talking ? "started" : "stopped", tx_frames);
    }
}

/* slot -1: us */
int voice_talking(int slot)
{
    double now = now_ms();

    if (slot < 0)
        return tx_last && now - tx_last < TALK_MS;
    if (slot >= MAX_PLAYERS)
        return 0;
    return spk[slot].last_rx && now - spk[slot].last_rx < TALK_MS;
}

/* ---- Lua ---- */

static double num_param(int n)
{
    unsigned int o = lua_getparam(n);
    return (o && lua_isnumber(o)) ? lua_getnumber(o) : 0.0;
}

/* coop_voice_test(ms): talk a test tone for ms instead of the mic (background test mode) */
static void l_coop_voice_test(void)
{
    tone_until = now_ms() + num_param(1);
}

/* coop_voice_info(slot) -> frames received from that player, their volume for us (%), frames lost */
static void l_coop_voice_info(void)
{
    int slot = (int)num_param(1);

    if (slot < 0 || slot >= MAX_PLAYERS)
        return;
    lua_pushnumber(spk[slot].frames);
    lua_pushnumber((int)(spk[slot].target * 100.0f + 0.5f));
    lua_pushnumber(spk[slot].lost);
}

static void l_coop_voice_talking(void)
{
    lua_pushnumber(voice_talking((int)num_param(1)));
}

void voice_lua_init(void)
{
    tolua_function(NULL, "coop_voice_test", l_coop_voice_test);
    tolua_function(NULL, "coop_voice_info", l_coop_voice_info);
    tolua_function(NULL, "coop_voice_talking", l_coop_voice_talking);
}
