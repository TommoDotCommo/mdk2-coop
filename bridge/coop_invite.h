/* Co-op invite codes (v1) and the one paste box recogniser.
 * Portable C99: no platform headers, no allocation, every buffer bounded by the caller.
 * Shared by MDK2 (bridge/) and The Witness (vendored copy); see docs/DISCOVERY.md. MIT licence.
 *
 * Code: "<GAME>-" + Crockford base32 of the payload, groups of 5 separated by '-'.
 * Payload: [0] version<<4 | flags (bit0 has-steam, bit1 invite-only); [1..4] lobby secret u32 LE;
 * [5..6] port u16 LE; [7] route count (0..4); count x (kind u8: 1 LAN, 2 Tailscale/VPN,
 * 3 internet; IPv4 4 bytes); if has-steam: Steam lobby id u64 LE; last byte CRC-8 (poly 0x07,
 * init 0) over every previous byte. Routes keep their order: the host lists them best first. */
#ifndef COOP_INVITE_H
#define COOP_INVITE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CI_VERSION 1
#define CI_MAX_ROUTES 4
#define CI_FLAG_STEAM 1u
#define CI_FLAG_INVITE_ONLY 2u
#define CI_PAYLOAD_MIN 9
#define CI_PAYLOAD_MAX (8 + CI_MAX_ROUTES * 5 + 8 + 1)
#define CI_CHARS_MAX ((CI_PAYLOAD_MAX * 8 + 4) / 5)
#define CI_CODE_MAX (8 + 1 + CI_CHARS_MAX + CI_CHARS_MAX / 5 + 1)
#define CI_NAME_MAX 32
#define CI_SCAN_MAX 65536

enum ci_kind { CI_LAN = 1, CI_VPN = 2, CI_INTERNET = 3 };
enum ci_error { CI_OK, CI_INCOMPLETE, CI_WRONG_GAME, CI_NEWER, CI_NOT_RECOGNISED };
struct ci_route { uint8_t kind, ip[4]; };
struct ci_invite {
    uint8_t flags;
    uint32_t secret;
    uint16_t port;
    int n;
    struct ci_route route[CI_MAX_ROUTES];
    uint64_t steam_lobby;
};
struct ci_game { const char *prefix, *product, *probe_id; };

/* Known games: invite prefix, product name used in messages, 4-byte probe game id. */
static inline const struct ci_game *ci_game_at(int i)
{
    static const struct ci_game games[] = { { "MDK2", "MDK2", "MDK2" }, { "WIT", "The Witness", "WIT " } };
    return i >= 0 && i < (int)(sizeof(games) / sizeof(games[0])) ? &games[i] : NULL;
}
static inline int ci_upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static inline int ci_alnum(int c) { return (c >= '0' && c <= '9') || (ci_upper(c) >= 'A' && ci_upper(c) <= 'Z'); }
static inline int ci_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
/* Index of the game whose prefix is exactly text[0..n), case-insensitive, or -1. */
static inline int ci_game_find(const char *text, size_t n)
{
    int i;
    const struct ci_game *g;
    for (i = 0; (g = ci_game_at(i)) != NULL; i++) {
        size_t k, len = strlen(g->prefix);
        if (len != n) continue;
        for (k = 0; k < n && ci_upper((unsigned char)text[k]) == g->prefix[k]; k++) {}
        if (k == n) return i;
    }
    return -1;
}

static inline uint8_t ci_crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    int k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

/* Payload bytes for an invite, or 0 when the invite is not encodable. */
static inline int ci_pack(const struct ci_invite *v, uint8_t *b)
{
    int n = 0, i, k;
    if (v->n < 0 || v->n > CI_MAX_ROUTES || (v->flags & ~3u) || !v->port) return 0;
    b[n++] = (uint8_t)(CI_VERSION << 4 | v->flags);
    for (k = 0; k < 4; k++) b[n++] = (uint8_t)(v->secret >> (8 * k));
    b[n++] = (uint8_t)v->port; b[n++] = (uint8_t)(v->port >> 8);
    b[n++] = (uint8_t)v->n;
    for (i = 0; i < v->n; i++) {
        if (v->route[i].kind < CI_LAN || v->route[i].kind > CI_INTERNET) return 0;
        b[n++] = v->route[i].kind;
        memcpy(b + n, v->route[i].ip, 4); n += 4;
    }
    if (v->flags & CI_FLAG_STEAM) for (k = 0; k < 8; k++) b[n++] = (uint8_t)(v->steam_lobby >> (8 * k));
    b[n] = ci_crc8(b, (size_t)n);
    return n + 1;
}
/* Transactional: *v changes only on CI_OK. */
static inline int ci_unpack(const uint8_t *b, size_t n, struct ci_invite *v)
{
    struct ci_invite t;
    size_t at = 8, need;
    int i, k;
    memset(&t, 0, sizeof(t));
    if (n < CI_PAYLOAD_MIN || n > CI_PAYLOAD_MAX || ci_crc8(b, n - 1) != b[n - 1]) return CI_INCOMPLETE;
    if (b[0] >> 4 > CI_VERSION) return CI_NEWER;
    if (b[0] >> 4 != CI_VERSION) return CI_INCOMPLETE;
    t.flags = (uint8_t)(b[0] & 3u);
    for (k = 0; k < 4; k++) t.secret |= (uint32_t)b[1 + k] << (8 * k);
    t.port = (uint16_t)(b[5] | b[6] << 8);
    t.n = b[7];
    if (t.n > CI_MAX_ROUTES || !t.port) return CI_INCOMPLETE;
    need = 8 + 5 * (size_t)t.n + (t.flags & CI_FLAG_STEAM ? 8 : 0) + 1;
    if (n != need) return CI_INCOMPLETE;
    for (i = 0; i < t.n; i++, at += 5) {
        if (b[at] < CI_LAN || b[at] > CI_INTERNET) return CI_INCOMPLETE;
        t.route[i].kind = b[at];
        memcpy(t.route[i].ip, b + at + 1, 4);
    }
    if (t.flags & CI_FLAG_STEAM) for (k = 0; k < 8; k++) t.steam_lobby |= (uint64_t)b[at + k] << (8 * k);
    *v = t;
    return CI_OK;
}

static inline int ci_b32_value(int c)
{
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    const char *p;
    c = ci_upper(c);
    if (c == 'I' || c == 'L') return 1;
    if (c == 'O') return 0;
    if (!c) return -1;
    p = strchr(alphabet, c);
    return p ? (int)(p - alphabet) : -1;
}
/* "<PREFIX>-XXXXX-XXXXX-..." into out (capacity cap); returns length or 0. */
static inline int ci_encode(const struct ci_invite *v, const char *prefix, char *out, size_t cap)
{
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    uint8_t b[CI_PAYLOAD_MAX];
    int n = ci_pack(v, b), bits = 0, chars = 0;
    uint32_t acc = 0;
    size_t used, i;
    if (!n || !prefix || ci_game_find(prefix, strlen(prefix)) < 0) return 0;
    used = strlen(prefix);
    if (used + 1 + CI_CHARS_MAX + CI_CHARS_MAX / 5 + 1 > cap) return 0;
    memcpy(out, prefix, used);
    out[used++] = '-';
    for (i = 0; i < (size_t)n || bits; ) {
        int value;
        if (bits < 5 && i < (size_t)n) { acc = acc << 8 | b[i++]; bits += 8; continue; }
        value = bits >= 5 ? (int)(acc >> (bits - 5)) & 31 : (int)(acc << (5 - bits)) & 31;
        bits = bits >= 5 ? bits - 5 : 0;
        acc &= (1u << bits) - 1;
        if (chars && chars % 5 == 0) out[used++] = '-';
        out[used++] = alphabet[value]; chars++;
    }
    out[used] = 0;
    return (int)used;
}
/* Decode a code for this game (its prefix). Whitespace and '-' inside the body are ignored.
 * *other names the product of a known other game's prefix (CI_WRONG_GAME). */
static inline int ci_decode(const char *text, size_t len, const char *game, struct ci_invite *v, const char **other)
{
    uint8_t b[CI_PAYLOAD_MAX];
    uint32_t acc = 0;
    size_t i = 0, p, chars = 0, nb = 0;
    int bits = 0, which, mine;
    if (other) *other = NULL;
    while (i < len && i <= 8 && text[i] != '-') i++;
    if (i == len || text[i] != '-' || !i) return CI_NOT_RECOGNISED;
    which = ci_game_find(text, i);
    if (which < 0) return CI_NOT_RECOGNISED;
    mine = game ? ci_game_find(game, strlen(game)) : -1;
    if (which != mine) { if (other) *other = ci_game_at(which)->product; return CI_WRONG_GAME; }
    for (p = i + 1; p < len; p++) {
        int c = (unsigned char)text[p], value;
        if (c == '-' || ci_space(c)) continue;
        value = ci_b32_value(c);
        if (value < 0 || ++chars > CI_CHARS_MAX) return CI_INCOMPLETE;
        acc = acc << 5 | (uint32_t)value; bits += 5;
        if (bits >= 8) { b[nb++] = (uint8_t)(acc >> (bits - 8)); bits -= 8; }
        acc &= (1u << bits) - 1;
    }
    /* Canonical length only: leftover padding is under 5 bits and zero. */
    if (bits >= 5 || acc) return CI_INCOMPLETE;
    return ci_unpack(b, nb, v);
}
static inline const char *ci_error_text(int error, const char *other, char *buf, size_t cap)
{
    if (!cap) return buf;
    if (error == CI_INCOMPLETE) snprintf(buf, cap, "This code is incomplete. Ask for the whole invite again.");
    else if (error == CI_WRONG_GAME) snprintf(buf, cap, "That invite is for %s.", other ? other : "another game");
    else if (error == CI_NEWER) snprintf(buf, cap, "This invite is from a newer version of the co-op mod. Update to join.");
    else if (error == CI_NOT_RECOGNISED) snprintf(buf, cap, "Not recognised. Paste an invite, a link, a Steam lobby number or an address.");
    else buf[0] = 0;
    return buf;
}

/* Byte length of the printable UTF-8 character at s[i] (n bytes in s), or 0: no control bytes,
 * no overlong forms, no surrogates. */
static inline size_t ci_char(const char *s, size_t n, size_t i)
{
    unsigned c = (unsigned char)s[i];
    size_t k, len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
    uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
    if (!len || i + len > n) return 0;
    for (k = 1; k < len; k++) {
        unsigned d = (unsigned char)s[i + k];
        if ((d & 0xc0) != 0x80) return 0;
        cp = cp << 6 | (d & 0x3f);
    }
    if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0) || (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    return len;
}
/* Bounded copy of a display name: printable UTF-8, at most CI_NAME_MAX bytes, cut on a character
 * boundary, trailing spaces dropped. Returns the length written (out has CI_NAME_MAX+1 bytes). */
static inline size_t ci_name_clean(const char *s, size_t n, char *out)
{
    size_t i = 0, used = 0, len;
    while (i < n && (len = ci_char(s, n, i)) != 0 && used + len <= CI_NAME_MAX) {
        memcpy(out + used, s + i, len); used += len; i += len;
    }
    while (used && out[used - 1] == ' ') used--;
    out[used] = 0;
    return used;
}
/* C3: "<name> is hosting <product>. Join: https://<join page>/#<CODE>". */
static inline int ci_message(const char *name, const char *product, const char *join_page, const char *code, char *out, size_t cap)
{
    char clean[CI_NAME_MAX + 1];
    int n;
    if (!ci_name_clean(name, strlen(name), clean)) return 0;
    n = snprintf(out, cap, "%s is hosting %s. Join: https://%s/#%s", clean, product, join_page, code);
    return n > 0 && (size_t)n < cap ? n : 0;
}

/* B8: find this game's invite anywhere in clipboard text. Fills *v and the inviter's name
 * when the text says "<name> is hosting". Only this game's codes count; nothing is kept. */
static inline int ci_find_invite(const char *text, size_t len, const char *game, struct ci_invite *v, char *name)
{
    size_t i, plen;
    int mine = game ? ci_game_find(game, strlen(game)) : -1;
    const char *prefix;
    name[0] = 0;
    if (mine < 0) return 0;
    prefix = ci_game_at(mine)->prefix; plen = strlen(prefix);
    if (len > CI_SCAN_MAX) len = CI_SCAN_MAX;
    for (i = 0; i + plen < len; i++) {
        size_t end = i, line, k;
        if ((i && ci_alnum((unsigned char)text[i - 1])) || ci_game_find(text + i, plen) != mine || text[i + plen] != '-') continue;
        while (end < len && end - i < CI_CODE_MAX && (ci_alnum((unsigned char)text[end]) || text[end] == '-')) end++;
        for (; end > i + plen + 1; ) {
            struct ci_invite t;
            if (ci_decode(text + i, end - i, game, &t, NULL) == CI_OK) {
                static const char marker[] = " is hosting ";
                for (line = i; line && text[line - 1] != '\n' && text[line - 1] != '\r'; line--) {}
                for (k = line; k + sizeof(marker) - 1 <= i; k++) {
                    if (!memcmp(text + k, marker, sizeof(marker) - 1)) {
                        size_t s = line;
                        while (s < k && ci_space((unsigned char)text[s])) s++;
                        if (k - s <= CI_NAME_MAX * 2) ci_name_clean(text + s, k - s, name);
                        break;
                    }
                }
                *v = t;
                return 1;
            }
            while (end > i + plen + 1 && text[end - 1] != '-') end--;
            if (end > i + plen + 1) end--;
        }
    }
    return 0;
}
static inline int ci_clipboard_prompt(const char *name, char *out, size_t cap)
{
    int n = name && name[0] ? snprintf(out, cap, "%s's invite is on your clipboard. Join?", name)
                            : snprintf(out, cap, "An invite is on your clipboard. Join?");
    return n > 0 && (size_t)n < cap ? n : 0;
}

/* ---- B7: the paste box ---- */
enum cr_kind { CR_EMPTY, CR_INVITE, CR_LINK, CR_STEAM_LOBBY, CR_ADDRESS, CR_HOST, CR_TAILSCALE, CR_ERROR };
#define CR_INPUT_MAX 2048
struct cr_result {
    int kind, error;
    const char *other;          /* product of another game's invite (CI_WRONG_GAME) */
    struct ci_invite invite;    /* CR_INVITE, CR_LINK */
    uint64_t lobby;             /* CR_STEAM_LOBBY */
    uint8_t ip[4];              /* CR_ADDRESS */
    unsigned port;              /* CR_ADDRESS, CR_HOST, CR_TAILSCALE; 0 = default port */
    char host[254];             /* CR_HOST, CR_TAILSCALE: lower case, no trailing dot */
    char code[33];              /* CR_ADDRESS: the Witness's address:port/CODE form */
};
static inline int cr_digits(const char *s, size_t n, uint64_t max, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;
    if (!n) return 0;
    for (i = 0; i < n; i++) {
        unsigned d = (unsigned char)s[i] - '0';
        if (d > 9 || v > (max - d) / 10) return 0;
        v = v * 10 + d;
    }
    *out = v;
    return 1;
}
/* ":port" at s[0..n) (n==0 means absent). */
static inline int cr_port(const char *s, size_t n, unsigned *port)
{
    uint64_t v;
    if (!n) { *port = 0; return 1; }
    if (s[0] != ':' || n > 6 || (n > 2 && s[1] == '0') || !cr_digits(s + 1, n - 1, 65535, &v) || !v) return 0;
    *port = (unsigned)v;
    return 1;
}
static inline int cr_ipv4(const char *s, size_t n, uint8_t ip[4], size_t *used)
{
    size_t i = 0;
    int part;
    for (part = 0; part < 4; part++) {
        size_t start = i;
        uint64_t v;
        while (i < n && i - start < 4 && s[i] >= '0' && s[i] <= '9') i++;
        if (i == start || i - start > 3 || (i - start > 1 && s[start] == '0') || !cr_digits(s + start, i - start, 255, &v)) return 0;
        ip[part] = (uint8_t)v;
        if (part < 3) { if (i >= n || s[i] != '.') return 0; i++; }
    }
    *used = i;
    return 1;
}
static inline int cr_hostname(const char *s, size_t n, char *out)
{
    size_t i, label = 0, labels = 1;
    int digits_only = 1;
    if (n && s[n - 1] == '.') n--;
    if (!n || n > 253) return 0;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c == '.') {
            if (!label || s[i - 1] == '-') return 0;
            out[i] = '.'; label = 0; labels++; digits_only = 1;
            continue;
        }
        if (!ci_alnum(c) && c != '-') return 0;
        if (c == '-' && !label) return 0;
        if (c < '0' || c > '9') digits_only = 0;
        if (++label > 63) return 0;
        out[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    if (!label || s[n - 1] == '-' || digits_only) return 0;
    out[n] = 0;
    return (int)labels;
}
/* Recognise one pasted text. Order: link, invite code, Steam lobby number, IPv4[:port][/CODE],
 * host name or Tailscale name. Returns r->kind. */
static inline int coop_recognise(const char *text, size_t len, const char *game, struct cr_result *r)
{
    size_t a = 0, b = len, i, used;
    const char *s;
    uint64_t lobby;
    memset(r, 0, sizeof(*r));
    while (a < b && ci_space((unsigned char)text[a])) a++;
    while (b > a && ci_space((unsigned char)text[b - 1])) b--;
    if (a == b) return r->kind = CR_EMPTY;
    s = text + a; len = b - a;
    if (len > CR_INPUT_MAX || memchr(s, 0, len)) { r->error = CI_NOT_RECOGNISED; return r->kind = CR_ERROR; }
    /* Links: the join page (code in the fragment) or the <mod>-coop:// handler. */
    for (i = 0; i < len && i < 32 && (ci_alnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.'); i++) {}
    if (i && i + 3 <= len && !memcmp(s + i, "://", 3)) {
        const char *hash = (const char *)memchr(s, '#', len), *code;
        size_t n;
        if (hash) { code = hash + 1; n = (size_t)(s + len - code); }
        else if (i > 5 && !memcmp(s + i - 5, "-coop", 5)) {
            code = s + len; while (code > s + i + 3 && code[-1] != '/') code--;
            n = (size_t)(s + len - code);
        } else { r->error = CI_NOT_RECOGNISED; return r->kind = CR_ERROR; }
        r->error = ci_decode(code, n, game, &r->invite, &r->other);
        return r->kind = r->error ? CR_ERROR : CR_LINK;
    }
    /* Invite codes: a known game prefix, '-', then at least one group's worth of code
     * characters (so a host called "wit-pc" stays a host name). */
    for (i = 0; i < len && i <= 8 && s[i] != '-'; i++) {}
    if (i < len && s[i] == '-' && ci_game_find(s, i) >= 0) {
        size_t k, chars = 0;
        for (k = i + 1; k < len; k++) if (s[k] != '-' && !ci_space((unsigned char)s[k])) chars++;
        if (chars < 5) i = len;
    }
    if (i < len && s[i] == '-' && ci_game_find(s, i) >= 0) {
        r->error = ci_decode(s, len, game, &r->invite, &r->other);
        return r->kind = r->error ? CR_ERROR : CR_INVITE;
    }
    if (len >= 17 && len <= 19 && cr_digits(s, len, UINT64_MAX, &lobby)) { r->lobby = lobby; return r->kind = CR_STEAM_LOBBY; }
    if (cr_ipv4(s, len, r->ip, &used)) {
        const char *slash = (const char *)memchr(s + used, '/', len - used);
        size_t port_end = slash ? (size_t)(slash - s) : len;
        if (cr_port(s + used, port_end - used, &r->port)) {
            size_t n = slash ? len - port_end - 1 : 0;
            if (!slash) return r->kind = CR_ADDRESS;
            if (n && n < sizeof(r->code)) {
                for (i = 0; i < n && ci_alnum((unsigned char)slash[1 + i]); i++) r->code[i] = slash[1 + i];
                if (i == n) { r->code[n] = 0; return r->kind = CR_ADDRESS; }
            }
        }
        memset(r, 0, sizeof(*r)); r->error = CI_NOT_RECOGNISED; return r->kind = CR_ERROR;
    }
    {
        const char *colon = (const char *)memchr(s, ':', len);
        size_t host_end = colon ? (size_t)(colon - s) : len;
        int labels;
        if (cr_port(s + host_end, len - host_end, &r->port) && (labels = cr_hostname(s, host_end, r->host)) > 0) {
            size_t n = strlen(r->host);
            if ((n > 7 && !strcmp(r->host + n - 7, ".ts.net")) || (labels == 1 && strcmp(r->host, "localhost")))
                return r->kind = CR_TAILSCALE;
            return r->kind = CR_HOST;
        }
    }
    memset(r, 0, sizeof(*r));
    r->error = CI_NOT_RECOGNISED;
    return r->kind = CR_ERROR;
}
/* What the box recognised, said before Join. */
static inline const char *cr_describe(const struct cr_result *r, char *buf, size_t cap)
{
    char port[24] = "";
    if (!cap) return buf;
    if (r->port) snprintf(port, sizeof(port), ", port %u", r->port);
    switch (r->kind) {
    case CR_EMPTY: snprintf(buf, cap, "Paste an invite, a link, a Steam lobby number or an address."); break;
    case CR_INVITE: case CR_LINK:
        snprintf(buf, cap, "%s: %d route%s%s, port %u%s.", r->kind == CR_LINK ? "Invite link" : "Invite code", r->invite.n,
                 r->invite.n == 1 ? "" : "s", r->invite.flags & CI_FLAG_STEAM ? " and a Steam lobby" : "", r->invite.port,
                 r->invite.flags & CI_FLAG_INVITE_ONLY ? ", invite only" : "");
        break;
    case CR_STEAM_LOBBY: snprintf(buf, cap, "Steam lobby %llu.", (unsigned long long)r->lobby); break;
    case CR_ADDRESS:
        snprintf(buf, cap, "Address %u.%u.%u.%u%s%s.", r->ip[0], r->ip[1], r->ip[2], r->ip[3], port, r->code[0] ? " with a lobby code" : "");
        break;
    case CR_HOST: snprintf(buf, cap, "Host name %s%s.", r->host, port); break;
    case CR_TAILSCALE: snprintf(buf, cap, "Tailscale name %s%s.", r->host, port); break;
    default: ci_error_text(r->error, r->other, buf, cap); break;
    }
    return buf;
}
#endif
