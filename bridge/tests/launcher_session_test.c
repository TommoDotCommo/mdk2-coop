#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "launcher_session.h"
static uint32_t rng = 15;
static uint32_t next(void) { rng = rng * 1664525u + 1013904223u; return rng; }
int main(void)
{
    struct launcher_session s, old;
    struct ls_comm_state host_comm, client_comm;
    uint32_t ids[3] = { 0x101u, 0x202u, 0x303u };
    char names[3][LS_NAME_MAX] = { "Hosty", "Alice", "Bob" };
    char c1[256], m1[256], clean[128];
    struct ls_voice_pkt vpkt, vbad;
    int16_t pcm_in[LS_VOICE_FRAME], pcm_out[LS_VOICE_FRAME], pcm_loud[LS_VOICE_FRAME];
    uint32_t relay_ids[LS_MAX_MEMBERS];
    int played = 0, rcount = 0, n, i;
    uint8_t vflags = 0;
    uint32_t now;

    ls_open(&s, 0, 100);
    assert(s.phase == LS_WAITING);
    old = s;
    assert(!ls_roster(&s, 200, 1, 0) && s.phase == old.phase);
    assert(!ls_roster(&s, 200, 3001, 1));
    assert(!ls_roster(&s, 200, -2, 1));
    assert(!ls_begin(&s, 200, 3000, 1));
    assert(ls_roster(&s, 200, -1, 1) && s.phase == LS_LOBBY);
    assert(!ls_begin(&s, 300, 3000, 1));
    ls_tick(&s, 4201); assert(s.phase == LS_LOST);
    assert(ls_roster(&s, 4300, -1, 1) && s.phase == LS_LOBBY);
    assert(ls_roster(&s, 4400, 3000, 1));
    assert(s.deadline == 7400);
    assert(ls_roster(&s, 4500, 3000, 1) && s.deadline == 7400);
    ls_tick(&s, 7400); assert(s.phase == LS_LOST); /* host disappeared during countdown */
    assert(ls_roster(&s, 8000, 3000, 1));
    assert(ls_roster(&s, 10500, 500, 1));
    ls_tick(&s, 10999); assert(s.phase == LS_COUNTDOWN);
    ls_tick(&s, 11000); assert(s.phase == LS_START);
    assert(!ls_roster(&s, 11001, -1, 1));
    ls_leave(&s); assert(s.phase == LS_OFF);
    assert(!ls_roster(&s, 12000, 0, 1));
    ls_tick(&s, 15000); assert(s.phase == LS_OFF);
    ls_open(&s, 1, 20);
    assert(!ls_roster(&s, 30, 0, 1));
    assert(!ls_begin(&s, 30, 3001, 1));
    assert(!ls_begin(&s, 30, -1, 1));
    assert(!ls_begin(&s, 30, 3000, 0));
    assert(ls_begin(&s, 40, 3000, 1));
    assert(!ls_begin(&s, 50, 3000, 1));
    ls_tick(&s, 3039); assert(s.phase == LS_COUNTDOWN);
    ls_tick(&s, 3040); assert(s.phase == LS_START);
    ls_open(&s, 0, UINT32_MAX - 100);
    assert(ls_roster(&s, UINT32_MAX - 50, 200, 1));
    ls_tick(&s, UINT32_MAX - 10); assert(s.phase == LS_COUNTDOWN);
    ls_tick(&s, 148); assert(s.phase == LS_COUNTDOWN);
    ls_tick(&s, 149); assert(s.phase == LS_START);
    ls_open(&s, 0, 0);
    assert(ls_roster(&s, 100, 3000, 1));
    assert(ls_roster(&s, 200, -1, 1) && s.phase == LS_LOBBY && !s.deadline);

    /* ---- Pregame text chat: sanitisation, membership, endpoint, flood limit, history cap ---- */
    assert(ls_sanitize_text(" \t\x01\x1fHello|World\r\n\x7f  ", clean, sizeof(clean), 1) == 11);
    assert(!strcmp(clean, "Hello_World"));
    assert(ls_sanitize_text("   \r\n\x02\x03 ", clean, sizeof(clean), 1) == 0);

    ls_open(&s, 1, 1000);
    ls_comm_init(&host_comm);
    ls_comm_sync_roster(&host_comm, 3, ids, names);

    struct launcher_session cs;
    ls_open(&cs, 0, 1000);
    assert(ls_roster(&cs, 1000, -1, 1));
    ls_comm_init(&client_comm);
    ls_comm_sync_roster(&client_comm, 3, ids, names);

    n = ls_chat_encode_c1(0x202u, 1, 3, "Ready for\x07 Level 1|2!", c1, sizeof(c1));
    assert(n > 0);
    /* Endpoint failure and non-member rejected */
    assert(!ls_chat_host_recv_c1(&s, &host_comm, 1000, c1, (size_t)n, 0, m1, sizeof(m1)));
    {
        char rogue[256];
        int rn = ls_chat_encode_c1(0x999u, 1, 2, "Intruder", rogue, sizeof(rogue));
        assert(rn > 0 && !ls_chat_host_recv_c1(&s, &host_comm, 1000, rogue, (size_t)rn, 1, m1, sizeof(m1)));
    }
    /* Oversized body rejected */
    {
        char over[256];
        memset(over, 'A', sizeof(over) - 1); over[sizeof(over) - 1] = 0;
        memcpy(over, "MDKL C1 202 99 1|", 17);
        assert(!ls_chat_host_recv_c1(&s, &host_comm, 1000, over, 17 + LS_CHAT_MAX + 1, 1, m1, sizeof(m1)));
    }
    /* Valid C1 accepted, sanitized, broadcast as M1 */
    n = ls_chat_host_recv_c1(&s, &host_comm, 1000, c1, (size_t)n, 1, m1, sizeof(m1));
    assert(n > 0 && host_comm.history_total == 1);
    assert(!strcmp(host_comm.history[0].name, "Alice") && !strcmp(host_comm.history[0].text, "Ready for Level 1_2!"));
    /* Client M1 validation */
    assert(!ls_chat_client_recv_m1(&cs, &client_comm, 1000, m1, (size_t)n, 0)); /* bad endpoint */
    {
        char rogue_m1[256];
        int rm = ls_chat_encode_m1(0x888u, 1, 1, "Ghost", "Hi", rogue_m1, sizeof(rogue_m1));
        assert(rm > 0 && !ls_chat_client_recv_m1(&cs, &client_comm, 1000, rogue_m1, (size_t)rm, 1));
    }
    assert(ls_chat_client_recv_m1(&cs, &client_comm, 1000, m1, (size_t)n, 1));
    assert(client_comm.history_total == 1 && !strcmp(client_comm.history[0].name, "Alice"));
    /* Duplicate seq rejected */
    assert(!ls_chat_host_recv_c1(&s, &host_comm, 1010, c1, strlen(c1), 1, m1, sizeof(m1)));
    assert(!ls_chat_client_recv_m1(&cs, &client_comm, 1010, m1, (size_t)n, 1));
    /* Flood limit: 4 messages in 4000 ms allowed, 5th rejected until window expires */
    for (i = 2; i <= LS_CHAT_BURST_MAX; i++) {
        int clen = ls_chat_encode_c1(0x202u, (uint16_t)i, 3, "burst", c1, sizeof(c1));
        assert(ls_chat_host_recv_c1(&s, &host_comm, 1100 + (uint32_t)i, c1, (size_t)clen, 1, m1, sizeof(m1)) > 0);
    }
    {
        int clen = ls_chat_encode_c1(0x202u, 5, 3, "flood!", c1, sizeof(c1));
        assert(!ls_chat_host_recv_c1(&s, &host_comm, 2000, c1, (size_t)clen, 1, m1, sizeof(m1)));
        /* After 4000 ms window, allowed again */
        assert(ls_chat_host_recv_c1(&s, &host_comm, 5005, c1, (size_t)clen, 1, m1, sizeof(m1)) > 0);
    }
    /* History cap ring buffer */
    for (i = 10; i < 10 + LS_CHAT_HISTORY + 5; i++) {
        int rm = ls_chat_encode_m1(0x101u, (uint16_t)i, (uint8_t)(i % LS_COLORS), "Hosty", "msg", m1, sizeof(m1));
        assert(ls_chat_client_recv_m1(&cs, &client_comm, (uint32_t)(i * 5000), m1, (size_t)rm, 1));
    }
    assert(client_comm.history_total == 1 + LS_CHAT_HISTORY + 5);
    assert(client_comm.history[(client_comm.history_total - 1) % LS_CHAT_HISTORY].seq == (uint16_t)(10 + LS_CHAT_HISTORY + 4));

    /* Regression: local host sends must not enter the client-only C1 receiver.
     * Both recipients and host history see identical sanitized messages. */
    {
        struct ls_comm_state hc, cc;
        struct launcher_session hs, js;
        ls_open(&hs, 1, 1000); ls_open(&js, 0, 1000);
        assert(ls_roster(&js, 1000, -1, 1));
        ls_comm_init(&hc); ls_comm_init(&cc);
        ls_comm_sync_roster(&hc, 3, ids, names); ls_comm_sync_roster(&cc, 3, ids, names);
        assert(!ls_chat_host_send(&js, &hc, 1000, ids[0], 1, 0, "client", m1, sizeof(m1)));
        assert(!ls_chat_host_send(&hs, &hc, 1000, ids[1], 1, 0, "wrong identity", m1, sizeof(m1)));
        hs.phase = LS_OFF;
        assert(!ls_chat_host_send(&hs, &hc, 1000, ids[0], 1, 0, "closed", m1, sizeof(m1)));
        hs.phase = LS_LOBBY;
        assert(!ls_chat_host_send(&hs, &hc, 1000, ids[0], 1, 0, "\t\r\n", m1, sizeof(m1)));
        assert(!ls_chat_host_send(&hs, &hc, 1000, ids[0], 1, 0, "small buffer", m1, 4));
        n = ls_chat_host_send(&hs, &hc, 1000, ids[0], 1, 0, "Host|hello\x01", m1, sizeof(m1));
        assert(n > 0 && hc.history_total == 1 && !strcmp(hc.history[0].text, "Host_hello"));
        assert(ls_chat_client_recv_m1(&js, &cc, 1000, m1, n, 1));
        assert(cc.history_total == 1 && !strcmp(cc.history[0].text, hc.history[0].text));
        assert(!strcmp(cc.history[0].name, names[0]));
        assert(!ls_chat_host_send(&hs, &hc, 1001, ids[0], 1, 0, "duplicate", m1, sizeof(m1)));
        for (i = 2; i <= LS_CHAT_BURST_MAX; i++)
            assert(ls_chat_host_send(&hs, &hc, 1010, ids[0], (uint16_t)i, 0, "burst", m1, sizeof(m1)));
        assert(!ls_chat_host_send(&hs, &hc, 1020, ids[0], 5, 0, "flood", m1, sizeof(m1)));
        assert(ls_chat_host_send(&hs, &hc, 5001, ids[0], 5, 0, "after window", m1, sizeof(m1)));
        n = ls_chat_encode_c1(ids[0], 6, 0, "spoof host", c1, sizeof(c1));
        assert(n > 0 && !ls_chat_host_recv_c1(&hs, &hc, 5002, c1, n, 1, m1, sizeof(m1)));
    }

    /* ---- Pregame voice chat: IMA ADPCM, VAD/PTT, member-only routing, mute, volume, speaking indicator ---- */
    for (i = 0; i < LS_VOICE_FRAME; i++) pcm_in[i] = (int16_t)((i % 32 < 16) ? 3000 : -3000);
    assert(ls_pcm_level(pcm_in, LS_VOICE_FRAME) == 3000);
    /* VAD below threshold does not send; above threshold sends and holds hangover */
    assert(!ls_voice_should_send(&client_comm, 1, 0, pcm_in, 5000, 1000, &vflags));
    assert(ls_voice_should_send(&client_comm, 1, 0, pcm_in, 1000, 1000, &vflags) && (vflags == LS_VOICE_FLAG_VAD));
    ls_voice_pack(&client_comm, 0x202u, vflags, pcm_in, &vpkt);

    /* Packet validation checks */
    assert(!ls_voice_validate(&vpkt, sizeof(vpkt) - 1, &vbad));
    vbad = vpkt; vbad.magic = 0; assert(!ls_voice_validate(&vbad, sizeof(vbad), &vbad));
    vbad = vpkt; vbad.version = 2; assert(!ls_voice_validate(&vbad, sizeof(vbad), &vbad));
    vbad = vpkt; vbad.index = 89; assert(!ls_voice_validate(&vbad, sizeof(vbad), &vbad));

    /* Host receives voice from Alice (0x202): relays only to Bob (0x303), plays locally */
    assert(!ls_voice_accept(&s, &host_comm, 1000, &vpkt, sizeof(vpkt), 0, 0x101u, pcm_out, &played, relay_ids, &rcount));
    assert(!ls_voice_accept(&s, &host_comm, 1000, &vpkt, sizeof(vpkt), 1, 0x202u, pcm_out, &played, relay_ids, &rcount)); /* self echo */
    {
        struct ls_voice_pkt outsider = vpkt;
        outsider.sender_id = 0x777u;
        assert(!ls_voice_accept(&s, &host_comm, 1000, &outsider, sizeof(outsider), 1, 0x101u, pcm_out, &played, relay_ids, &rcount));
    }
    assert(ls_voice_accept(&s, &host_comm, 1000, &vpkt, sizeof(vpkt), 1, 0x101u, pcm_out, &played, relay_ids, &rcount));
    assert(played == 1 && rcount == 1 && relay_ids[0] == 0x303u);
    assert(ls_member_talking(ls_comm_find(&host_comm, 0x202u), 1100));
    assert(!ls_member_talking(ls_comm_find(&host_comm, 0x202u), 1300));
    /* Duplicate voice seq rejected */
    assert(!ls_voice_accept(&s, &host_comm, 1020, &vpkt, sizeof(vpkt), 1, 0x101u, pcm_out, &played, relay_ids, &rcount));

    /* Per-player volume scaling (200% vs 100%) and per-player mute */
    ls_comm_find(&host_comm, 0x202u)->volume = 200;
    client_comm.enc_pred = 0; client_comm.enc_index = 0;
    ls_voice_pack(&client_comm, 0x202u, 0, pcm_in, &vpkt);
    assert(ls_voice_accept(&s, &host_comm, 1040, &vpkt, sizeof(vpkt), 1, 0x101u, pcm_loud, &played, relay_ids, &rcount));
    assert(played == 1 && ls_pcm_level(pcm_loud, LS_VOICE_FRAME) > ls_pcm_level(pcm_out, LS_VOICE_FRAME));

    ls_comm_find(&host_comm, 0x202u)->muted = 1;
    ls_voice_pack(&client_comm, 0x202u, 0, pcm_in, &vpkt);
    assert(ls_voice_accept(&s, &host_comm, 1080, &vpkt, sizeof(vpkt), 1, 0x101u, pcm_loud, &played, relay_ids, &rcount));
    assert(played == 0 && ls_pcm_level(pcm_loud, LS_VOICE_FRAME) == 0);

    /* FLAG_LAST clears speaking indicator immediately */
    ls_comm_find(&host_comm, 0x202u)->muted = 0;
    ls_voice_pack(&client_comm, 0x202u, LS_VOICE_FLAG_LAST, pcm_in, &vpkt);
    assert(ls_voice_accept(&s, &host_comm, 1100, &vpkt, sizeof(vpkt), 1, 0x101u, pcm_out, &played, relay_ids, &rcount));
    assert(!ls_member_talking(ls_comm_find(&host_comm, 0x202u), 1101));

    /* Deterministic random-loss/event/fuzzer simulator (100000 sessions + fuzzed chat/voice datagrams). */
    for (i = 0; i < 100000; i++) {
        int valid = next() & 1, delay = (int)(next() % 5000) - 1000;
        now = next();
        ls_open(&s, next() & 1, now);
        old = s;
        if (s.host) {
            int accepted = ls_begin(&s, now, delay, valid);
            assert(accepted == (valid && delay >= 0 && delay <= 3000));
            if (!accepted) assert(s.phase == old.phase);
        } else {
            int accepted = ls_roster(&s, now, delay, valid);
            assert(accepted == (valid && delay >= -1 && delay <= 3000));
            if (!accepted) assert(s.phase == old.phase);
        }
        if (i < 5000) {
            unsigned char fuzz[360];
            size_t flen = next() % sizeof(fuzz), j;
            for (j = 0; j < flen; j++) fuzz[j] = (unsigned char)next();
            (void)ls_chat_host_recv_c1(&s, &host_comm, now, (const char *)fuzz, flen, 1, m1, sizeof(m1));
            (void)ls_chat_client_recv_m1(&cs, &client_comm, now, (const char *)fuzz, flen, 1);
            (void)ls_voice_accept(&s, &host_comm, now, fuzz, flen, 1, 0x101u, pcm_out, &played, relay_ids, &rcount);
        }
        now += next() % 8000;
        ls_tick(&s, now);
        if (s.phase == LS_START && !s.host) assert((uint32_t)(now - s.seen) <= 1000);
        ls_leave(&s); ls_tick(&s, now + 10000); assert(s.phase == LS_OFF);
    }
    puts("PASS: lifecycle transitions, pregame chat/voice simulator, fuzzer and 100000 simulated sessions");
    return 0;
}
