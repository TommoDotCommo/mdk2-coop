/* T0: invite code v1, the paste box recogniser and the clipboard finder, with fuzzing.
 * Usage: coop_invite_test [fuzz iterations] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "coop_invite.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static uint32_t seed = 0x2545f491u;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

/* Raw payload bytes to a code, bypassing ci_pack (to build deliberately wrong payloads). */
static void raw_code(const char *prefix, const uint8_t *b, size_t n, char *out)
{
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    size_t used = strlen(prefix), i, bit, chars = 0;
    memcpy(out, prefix, used); out[used++] = '-';
    for (bit = 0; bit < n * 8; bit += 5) {
        int v = 0, k;
        for (k = 0; k < 5; k++) {
            size_t at = bit + (size_t)k;
            v = v << 1 | (at < n * 8 ? (b[at / 8] >> (7 - at % 8)) & 1 : 0);
        }
        if (chars && chars % 5 == 0) out[used++] = '-';
        out[used++] = alphabet[v]; chars++;
    }
    (void)i;
    out[used] = 0;
}
static int same(const struct ci_invite *a, const struct ci_invite *b)
{
    int i;
    if (a->flags != b->flags || a->secret != b->secret || a->port != b->port || a->n != b->n) return 0;
    if ((a->flags & CI_FLAG_STEAM) && a->steam_lobby != b->steam_lobby) return 0;
    for (i = 0; i < a->n; i++) if (a->route[i].kind != b->route[i].kind || memcmp(a->route[i].ip, b->route[i].ip, 4)) return 0;
    return 1;
}
static void random_invite(struct ci_invite *v)
{
    int i;
    memset(v, 0, sizeof(*v));
    v->flags = (uint8_t)(rnd() & 3); v->secret = rnd(); v->port = (uint16_t)(1 + rnd() % 65535);
    v->n = (int)(rnd() % 5);
    for (i = 0; i < v->n; i++) {
        uint32_t ip = rnd();
        v->route[i].kind = (uint8_t)(1 + rnd() % 3);
        memcpy(v->route[i].ip, &ip, 4);
    }
    if (v->flags & CI_FLAG_STEAM) v->steam_lobby = (uint64_t)rnd() << 32 | rnd();
}
static int well_formed(const char *code, const char *prefix)
{
    size_t p = strlen(prefix), i, group = 0;
    if (strncmp(code, prefix, p) || code[p] != '-') return 0;
    for (i = p + 1; code[i]; i++) {
        if (code[i] == '-') { if (group != 5) return 0; group = 0; continue; }
        if (!strchr("0123456789ABCDEFGHJKMNPQRSTVWXYZ", code[i])) return 0;
        group++;
    }
    return group >= 1 && group <= 5;
}

int main(int argc, char **argv)
{
    long fuzz = argc > 1 ? atol(argv[1]) : 20000, it;
    struct ci_invite v, w, keep;
    struct cr_result r;
    char code[CI_CODE_MAX], text[4096], buf[256], name[CI_NAME_MAX + 1];
    const char *other;
    uint8_t b[CI_PAYLOAD_MAX];
    int n, i, k, accepted = 0, mutated = 0;

    /* CRC-8 poly 0x07 init 0: the standard check value. */
    CHECK(ci_crc8((const uint8_t *)"123456789", 9) == 0xf4);

    /* A fixed invite: three routes in the host's order, Steam lobby, invite-only. */
    memset(&v, 0, sizeof(v));
    v.flags = CI_FLAG_STEAM | CI_FLAG_INVITE_ONLY; v.secret = 0xdeadbeef; v.port = 27900; v.n = 3;
    v.route[0].kind = CI_LAN; memcpy(v.route[0].ip, "\xc0\xa8\x01\x05", 4);
    v.route[1].kind = CI_VPN; memcpy(v.route[1].ip, "\x64\x65\x66\x67", 4);
    v.route[2].kind = CI_INTERNET; memcpy(v.route[2].ip, "\x01\x02\x03\x04", 4);
    v.steam_lobby = 109775241234567890ull;
    n = ci_encode(&v, "MDK2", code, sizeof(code));
    CHECK(n > 0 && (size_t)n == strlen(code) && well_formed(code, "MDK2"));
    CHECK(ci_decode(code, strlen(code), "MDK2", &w, &other) == CI_OK && same(&v, &w));
    CHECK(w.route[0].kind == CI_LAN && w.route[1].kind == CI_VPN && w.route[2].kind == CI_INTERNET);
    CHECK(w.route[0].ip[0] == 192 && w.route[1].ip[0] == 100 && w.route[2].ip[0] == 1);
    CHECK(ci_pack(&v, b) == 8 + 15 + 8 + 1 && b[0] == 0x13 && b[1] == 0xef && b[5] == (27900 & 255) && b[7] == 3);
    /* the same code for The Witness, lower case, aliases, spaces and missing dashes */
    n = ci_encode(&v, "WIT", code, sizeof(code));
    CHECK(n > 0 && well_formed(code, "WIT") && ci_decode(code, strlen(code), "WIT", &w, NULL) == CI_OK && same(&v, &w));
    CHECK(ci_decode(code, strlen(code), "MDK2", &w, &other) == CI_WRONG_GAME && other && !strcmp(other, "The Witness"));
    n = ci_encode(&v, "MDK2", code, sizeof(code));
    strcpy(text, code);
    for (i = 0; text[i]; i++) if (text[i] >= 'A' && text[i] <= 'Z') text[i] = (char)(text[i] + 32);
    CHECK(ci_decode(text, strlen(text), "mdk2", &w, NULL) == CI_OK && same(&v, &w));
    strcpy(text, code);
    for (i = 5, k = 0; text[i]; i++) if (text[i] == '1') text[i] = (k++ & 1) ? 'I' : 'l'; else if (text[i] == '0') text[i] = 'o';
    CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_OK && same(&v, &w));
    for (i = 0, k = 0; code[i]; i++) if (code[i] != '-' || i < 5) text[k++] = code[i]; else text[k++] = ' ';
    text[k] = 0;
    CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_OK && same(&v, &w));

    /* errors: every one transactional (w untouched) */
    keep = w;
    CHECK(ci_decode(code, strlen(code) - 1, "MDK2", &w, NULL) == CI_INCOMPLETE);
    CHECK(ci_decode(code, 12, "MDK2", &w, NULL) == CI_INCOMPLETE);
    sprintf(text, "%s0", code); CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_INCOMPLETE);
    sprintf(text, "%sU", code); CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_INCOMPLETE);
    sprintf(text, "%s!", code); CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_INCOMPLETE);
    CHECK(ci_decode("MDK2-", 5, "MDK2", &w, NULL) == CI_INCOMPLETE);
    CHECK(ci_decode("FOO-ABCDE", 9, "MDK2", &w, &other) == CI_NOT_RECOGNISED && !other);
    CHECK(ci_decode("MDK2ABCDE", 9, "MDK2", &w, NULL) == CI_NOT_RECOGNISED);
    CHECK(memcmp(&w, &keep, sizeof(w)) == 0);
    n = ci_pack(&v, b);
    for (k = 0, i = b[n - 1]; k < 256; k++) {           /* every wrong CRC */
        if (k == i) continue;
        b[n - 1] = (uint8_t)k; raw_code("MDK2", b, (size_t)n, text);
        CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_INCOMPLETE);
    }
    n = ci_pack(&v, b);
    b[0] = 0x23; b[n - 1] = ci_crc8(b, (size_t)n - 1); raw_code("MDK2", b, (size_t)n, text);
    CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_NEWER);
    b[0] = 0x03; b[n - 1] = ci_crc8(b, (size_t)n - 1); raw_code("MDK2", b, (size_t)n, text);
    CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_INCOMPLETE);
    n = ci_pack(&v, b); b[7] = 5; b[n - 1] = ci_crc8(b, (size_t)n - 1); raw_code("MDK2", b, (size_t)n, text);
    CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_INCOMPLETE);
    n = ci_pack(&v, b); b[8] = 0; b[n - 1] = ci_crc8(b, (size_t)n - 1); raw_code("MDK2", b, (size_t)n, text);
    CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_INCOMPLETE);
    n = ci_pack(&v, b); b[8] = 4; b[n - 1] = ci_crc8(b, (size_t)n - 1); raw_code("MDK2", b, (size_t)n, text);
    CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_INCOMPLETE);
    n = ci_pack(&v, b); b[5] = b[6] = 0; b[n - 1] = ci_crc8(b, (size_t)n - 1); raw_code("MDK2", b, (size_t)n, text);
    CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_INCOMPLETE);
    n = ci_pack(&v, b); b[0] &= (uint8_t)~CI_FLAG_STEAM; b[n - 1] = ci_crc8(b, (size_t)n - 1); raw_code("MDK2", b, (size_t)n, text);
    CHECK(ci_decode(text, strlen(text), "MDK2", &w, NULL) == CI_INCOMPLETE);   /* length no longer matches */
    /* nonzero padding bits: 9-byte payload = 72 bits = 15 chars with 3 padding bits */
    memset(&w, 0, sizeof(w)); w.port = 1;
    n = ci_encode(&w, "MDK2", code, sizeof(code));
    CHECK(n > 0 && strlen(code) == 5 + 15 + 2);
    strcpy(text, code); text[strlen(text) - 1] = (char)(text[strlen(text) - 1] == '0' ? '1' : '0');
    CHECK(ci_decode(text, strlen(text), "MDK2", &keep, NULL) != CI_OK);
    /* encoder refusals */
    w = v; w.n = 5; CHECK(!ci_encode(&w, "MDK2", code, sizeof(code)));
    w = v; w.route[1].kind = 4; CHECK(!ci_encode(&w, "MDK2", code, sizeof(code)));
    w = v; w.port = 0; CHECK(!ci_encode(&w, "MDK2", code, sizeof(code)));
    w = v; w.flags = 4; CHECK(!ci_encode(&w, "MDK2", code, sizeof(code)));
    CHECK(!ci_encode(&v, "FOO", code, sizeof(code)) && !ci_encode(&v, "MDK2", code, 20));
    /* wording */
    CHECK(!strcmp(ci_error_text(CI_INCOMPLETE, NULL, buf, sizeof(buf)), "This code is incomplete. Ask for the whole invite again."));
    CHECK(!strcmp(ci_error_text(CI_WRONG_GAME, "The Witness", buf, sizeof(buf)), "That invite is for The Witness."));
    CHECK(strstr(ci_error_text(CI_NOT_RECOGNISED, NULL, buf, sizeof(buf)), "Not recognised") == buf);

    /* C3 message and B8 clipboard */
    ci_encode(&v, "MDK2", code, sizeof(code));
    CHECK(ci_message("Fox", "MDK2", "example.github.io/mdk2-coop/join", code, text, sizeof(text)) > 0);
    sprintf(buf, "Fox is hosting MDK2. Join: https://example.github.io/mdk2-coop/join/#%s", code);
    CHECK(!strcmp(text, buf));
    CHECK(ci_message("\n\t", "MDK2", "x", code, buf, sizeof(buf)) == 0);
    CHECK(ci_message("Fox", "MDK2", "x", code, buf, 10) == 0);
    sprintf(text, "hey!\r\nFox is hosting MDK2. Join: https://example.github.io/j/#%s.\r\nsee you", code);
    CHECK(ci_find_invite(text, strlen(text), "MDK2", &w, name) && same(&v, &w) && !strcmp(name, "Fox"));
    CHECK(ci_clipboard_prompt(name, buf, sizeof(buf)) && !strcmp(buf, "Fox's invite is on your clipboard. Join?"));
    CHECK(!ci_find_invite(text, strlen(text), "WIT", &w, name));
    sprintf(text, "code %s-THANKS", code);
    CHECK(ci_find_invite(text, strlen(text), "MDK2", &w, name) && same(&v, &w) && !name[0]);
    CHECK(ci_clipboard_prompt(name, buf, sizeof(buf)) && !strcmp(buf, "An invite is on your clipboard. Join?"));
    sprintf(text, "X%s", code); CHECK(!ci_find_invite(text, strlen(text), "MDK2", &w, name));
    sprintf(text, "MDK2-ZZZZZ broken, real one: %s", code);
    CHECK(ci_find_invite(text, strlen(text), "MDK2", &w, name) && same(&v, &w));
    ci_encode(&v, "WIT", buf, sizeof(buf)); CHECK(!ci_find_invite(buf, strlen(buf), "MDK2", &w, name));
    sprintf(text, "\x01\x02\xff Fox\x07 is hosting MDK2. %s", code);
    CHECK(ci_find_invite(text, strlen(text), "MDK2", &w, name) && !strchr(name, 7) && !strchr(name, 1));

    /* B7 paste box */
    CHECK(coop_recognise("  \r\n", 4, "MDK2", &r) == CR_EMPTY);
    CHECK(coop_recognise(code, strlen(code), "MDK2", &r) == CR_INVITE && same(&v, &r.invite));
    sprintf(text, "  %s\n", code); CHECK(coop_recognise(text, strlen(text), "MDK2", &r) == CR_INVITE);
    sprintf(text, "https://example.github.io/mdk2-coop/join/#%s", code);
    CHECK(coop_recognise(text, strlen(text), "MDK2", &r) == CR_LINK && same(&v, &r.invite));
    sprintf(text, "mdk2-coop://join/%s", code); CHECK(coop_recognise(text, strlen(text), "MDK2", &r) == CR_LINK);
    sprintf(text, "mdk2-coop://join/#%s", code); CHECK(coop_recognise(text, strlen(text), "MDK2", &r) == CR_LINK);
    CHECK(coop_recognise("https://example.org/", 20, "MDK2", &r) == CR_ERROR && r.error == CI_NOT_RECOGNISED);
    sprintf(text, "https://x/#%s", buf);         /* buf holds the WIT code */
    CHECK(coop_recognise(text, strlen(text), "MDK2", &r) == CR_ERROR && r.error == CI_WRONG_GAME && !strcmp(r.other, "The Witness"));
    CHECK(coop_recognise(buf, strlen(buf), "MDK2", &r) == CR_ERROR && r.error == CI_WRONG_GAME);
    CHECK(coop_recognise(buf, strlen(buf), "WIT", &r) == CR_INVITE);
    CHECK(coop_recognise(code, 14, "MDK2", &r) == CR_ERROR && r.error == CI_INCOMPLETE);
    CHECK(!strcmp(cr_describe(&r, text, sizeof(text)), "This code is incomplete. Ask for the whole invite again."));
    CHECK(coop_recognise("109775241234567890", 18, "MDK2", &r) == CR_STEAM_LOBBY && r.lobby == 109775241234567890ull);
    CHECK(coop_recognise("12345678901234567", 17, "MDK2", &r) == CR_STEAM_LOBBY);
    CHECK(coop_recognise("9999999999999999999", 19, "MDK2", &r) == CR_STEAM_LOBBY);
    CHECK(coop_recognise("1234567890123456", 16, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("12345678901234567890", 20, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("192.168.1.5", 11, "MDK2", &r) == CR_ADDRESS && r.ip[0] == 192 && r.ip[3] == 5 && !r.port);
    CHECK(coop_recognise("10.0.0.1:27901", 14, "MDK2", &r) == CR_ADDRESS && r.port == 27901);
    CHECK(coop_recognise("10.0.0.1:27901/AB12cd", 21, "WIT", &r) == CR_ADDRESS && r.port == 27901 && !strcmp(r.code, "AB12cd"));
    CHECK(coop_recognise("10.0.0.1/XYZ", 12, "WIT", &r) == CR_ADDRESS && !r.port && !strcmp(r.code, "XYZ"));
    CHECK(!strcmp(cr_describe(&r, text, sizeof(text)), "Address 10.0.0.1 with a lobby code."));
    CHECK(coop_recognise("10.0.0.1:0", 10, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("10.0.0.1:65536", 14, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("10.0.0.1:027900", 15, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("10.0.0.01", 9, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("256.0.0.1", 9, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("10.0.0", 6, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("10.0.0.1/", 9, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("10.0.0.1/a-b", 12, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("Example.ORG", 11, "MDK2", &r) == CR_HOST && !strcmp(r.host, "example.org"));
    CHECK(coop_recognise("example.org.:28000", 18, "MDK2", &r) == CR_HOST && r.port == 28000 && !strcmp(r.host, "example.org"));
    CHECK(coop_recognise("fox-pc", 6, "MDK2", &r) == CR_TAILSCALE && !strcmp(r.host, "fox-pc"));
    CHECK(!strcmp(cr_describe(&r, text, sizeof(text)), "Tailscale name fox-pc."));
    CHECK(coop_recognise("fox-pc.tail1234.ts.net:27900", 28, "MDK2", &r) == CR_TAILSCALE && r.port == 27900);
    CHECK(coop_recognise("localhost", 9, "MDK2", &r) == CR_HOST);
    CHECK(coop_recognise("my-pc.local", 11, "MDK2", &r) == CR_HOST);
    CHECK(coop_recognise("-bad", 4, "MDK2", &r) == CR_ERROR && coop_recognise("bad-", 4, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("a..b", 4, "MDK2", &r) == CR_ERROR && coop_recognise("1.2.3.4.5", 9, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("fox pc", 6, "MDK2", &r) == CR_ERROR && coop_recognise("host:", 5, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("[::1]:27900", 11, "MDK2", &r) == CR_ERROR);
    CHECK(coop_recognise("my-pc-2", 7, "MDK2", &r) == CR_TAILSCALE);       /* not a known game prefix */
    CHECK(coop_recognise("wit-pc", 6, "MDK2", &r) == CR_TAILSCALE);       /* too short to be a code */
    CHECK(coop_recognise("MDK2-ABCDE", 10, "MDK2", &r) == CR_ERROR && r.error == CI_INCOMPLETE);
    CHECK(coop_recognise("WIT-ABCDE-FG", 12, "MDK2", &r) == CR_ERROR && r.error == CI_WRONG_GAME);
    coop_recognise(code, strlen(code), "MDK2", &r);
    CHECK(!strcmp(cr_describe(&r, text, sizeof(text)), "Invite code: 3 routes and a Steam lobby, port 27900, invite only."));
    memset(text, 'a', 3000); text[3000] = 0;
    CHECK(coop_recognise(text, 3000, "MDK2", &r) == CR_ERROR);

    /* fuzz 1: round trips of random invites, and single-character substitutions always rejected */
    for (it = 0; it < fuzz; it++) {
        char bad[CI_CODE_MAX];
        size_t len, at;
        random_invite(&v);
        n = ci_encode(&v, (rnd() & 1) ? "MDK2" : "WIT", code, sizeof(code));
        CHECK(n > 0 && well_formed(code, code[0] == 'M' ? "MDK2" : "WIT"));
        CHECK(ci_decode(code, (size_t)n, code[0] == 'M' ? "MDK2" : "WIT", &w, NULL) == CI_OK && same(&v, &w));
        len = strlen(code); strcpy(bad, code);
        do at = 5 + rnd() % (len - 5); while (bad[at] == '-');
        {
            int value = ci_b32_value(bad[at]), next = (value + 1 + (int)(rnd() % 31)) % 32;
            bad[at] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"[next];
        }
        CHECK(ci_decode(bad, len, code[0] == 'M' ? "MDK2" : "WIT", &w, NULL) != CI_OK);
        /* fuzz 2: several random mutations (substitute, delete, insert); count false accepts */
        strcpy(bad, code);
        for (k = 1 + (int)(rnd() % 3); k > 0; k--) {
            len = strlen(bad); at = rnd() % len;
            if ((rnd() % 3) == 0 && len > 1) memmove(bad + at, bad + at + 1, len - at);
            else if ((rnd() % 2) == 0 && len + 1 < sizeof(bad)) { memmove(bad + at + 1, bad + at, len - at + 1); bad[at] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ-"[rnd() % 33]; }
            else bad[at] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ-"[rnd() % 33];
        }
        if (strcmp(bad, code)) {
            mutated++;
            if (ci_decode(bad, strlen(bad), code[0] == 'M' ? "MDK2" : "WIT", &w, NULL) == CI_OK && !same(&v, &w)) accepted++;
        }
        /* fuzz 3: random bytes into every entry point: must not crash (sanitisers watch) */
        len = rnd() % 200;
        for (at = 0; at < len; at++) text[at] = (char)(rnd() & 255);
        if (rnd() & 1) memcpy(text, "MDK2-", len < 5 ? len : 5);
        text[len] = 0;
        ci_decode(text, len, "MDK2", &w, &other);
        coop_recognise(text, len, "MDK2", &r); cr_describe(&r, buf, sizeof(buf));
        ci_find_invite(text, len, "MDK2", &w, name);
        n = ci_pack(&v, b);
        for (at = 0; at < (size_t)n; at++) if (rnd() % 4 == 0) b[at] = (uint8_t)rnd();
        if (ci_unpack(b, (size_t)n, &w) == CI_OK) CHECK(ci_crc8(b, (size_t)n - 1) == b[n - 1]);
    }
    CHECK(fuzz < 1000 || accepted * 200 < mutated);   /* under 0.5% of random corruptions slip past CRC-8 */
    printf("fuzz: %ld invites, %d corrupted codes, %d accepted as another invite\n", fuzz, mutated, accepted);
    if (failures) { printf("FAILED: %d checks\n", failures); return 1; }
    puts("PASS: invite v1 codec, wording, clipboard finder, paste box recogniser, fuzzing");
    return 0;
}
