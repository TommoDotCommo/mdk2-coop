/* Portable GitHub release checker, SHA-256/ZIP verifier and transactional self-updater.
 * Pure functions and standard file operations only; no UI or game dependencies. */
#ifndef LAUNCHER_UPDATE_H
#define LAUNCHER_UPDATE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
static inline int lu_rename_replace(const char *src, const char *dst)
{
    return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
#else
static inline int lu_rename_replace(const char *src, const char *dst)
{
    return rename(src, dst) == 0;
}
#endif

#define LU_INTERVAL_SEC     86400u
#define LU_MAX_JSON_BYTES   65536u
#define LU_MAX_ASSET_BYTES  (32u * 1024u * 1024u)
#define LU_NOTES_MAX        1024u
#define LU_URL_MAX          384u
#define LU_TAG_MAX          48u

enum lu_result {
    LU_OK = 0,
    LU_UP_TO_DATE,
    LU_UPDATE_AVAILABLE,
    LU_THROTTLED,
    LU_ERR_OFFLINE,
    LU_ERR_RATELIMIT,
    LU_ERR_HTTP,
    LU_ERR_JSON,
    LU_ERR_VERSION,
    LU_ERR_ASSET,
    LU_ERR_SIDECAR,
    LU_ERR_TRUNCATED,
    LU_ERR_CHECKSUM,
    LU_ERR_ARCHIVE,
    LU_ERR_PE,
    LU_ERR_IO,
    LU_ERR_ROLLBACK
};

struct lu_schedule {
    int auto_enabled;
    uint64_t last_check;
    uint64_t retry_after;
};

struct lu_release {
    char tag[LU_TAG_MAX];
    char zip_name[96];
    char zip_url[LU_URL_MAX];
    size_t zip_size;
    char sha_name[112];
    char sha_url[LU_URL_MAX];
    size_t sha_size;
    char notes[LU_NOTES_MAX];
};

/* ---- FIPS 180-4 SHA-256 ---- */

static inline uint32_t lu_rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static inline void lu_sha256_block(uint32_t h[8], const unsigned char b[64])
{
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfc,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffa,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64], a = h[0], b0 = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i*4]<<24) | ((uint32_t)b[i*4+1]<<16) | ((uint32_t)b[i*4+2]<<8) | (uint32_t)b[i*4+3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = lu_rotr32(w[i-15], 7) ^ lu_rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = lu_rotr32(w[i-2], 17) ^ lu_rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    for (i = 0; i < 64; i++) {
        uint32_t s1 = lu_rotr32(e, 6) ^ lu_rotr32(e, 11) ^ lu_rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + s1 + ch + k[i] + w[i];
        uint32_t s0 = lu_rotr32(a, 2) ^ lu_rotr32(a, 13) ^ lu_rotr32(a, 22);
        uint32_t maj = (a & b0) ^ (a & c) ^ (b0 & c);
        uint32_t t2 = s0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b0; b0 = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b0; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static inline void lu_sha256_hex(const void *data, size_t len, char out_hex[65])
{
    static const char hex[] = "0123456789abcdef";
    uint32_t h[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    const unsigned char *p = (const unsigned char *)data;
    unsigned char tail[128];
    uint64_t bits = (uint64_t)len * 8u;
    size_t rem = len & 63u, pad_len = (rem < 56u) ? (64u - rem) : (128u - rem);
    int i;
    while (len >= 64u) {
        lu_sha256_block(h, p);
        p += 64u; len -= 64u;
    }
    memset(tail, 0, sizeof(tail));
    if (rem) memcpy(tail, p, rem);
    tail[rem] = 0x80u;
    for (i = 0; i < 8; i++)
        tail[rem + pad_len - 1 - i] = (unsigned char)(bits >> (i * 8));
    lu_sha256_block(h, tail);
    if (rem + pad_len > 64u)
        lu_sha256_block(h, tail + 64u);
    for (i = 0; i < 8; i++) {
        out_hex[i*8+0] = hex[(h[i] >> 28) & 15];
        out_hex[i*8+1] = hex[(h[i] >> 24) & 15];
        out_hex[i*8+2] = hex[(h[i] >> 20) & 15];
        out_hex[i*8+3] = hex[(h[i] >> 16) & 15];
        out_hex[i*8+4] = hex[(h[i] >> 12) & 15];
        out_hex[i*8+5] = hex[(h[i] >> 8) & 15];
        out_hex[i*8+6] = hex[(h[i] >> 4) & 15];
        out_hex[i*8+7] = hex[h[i] & 15];
    }
    out_hex[64] = 0;
}

static inline uint32_t lu_crc32(const unsigned char *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ---- Version comparison ---- */

struct lu_semver {
    unsigned major, minor, patch;
    char pre[32];
};

static inline int lu_tag_safe(const char *s)
{
    size_t i, n;
    if (!s || !*s) return 0;
    n = strlen(s);
    if (n >= LU_TAG_MAX || !isalnum((unsigned char)s[0])) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '+' && c != '-') return 0;
    }
    return 1;
}

static inline int lu_parse_semver(const char *tag, struct lu_semver *v)
{
    const char *p = tag;
    unsigned parts[3] = {0, 0, 0};
    int k;
    memset(v, 0, sizeof(*v));
    if (!lu_tag_safe(tag)) return 0;
    if (*p == 'v' || *p == 'V') p++;
    for (k = 0; k < 3; k++) {
        unsigned val = 0;
        if (!isdigit((unsigned char)*p)) return 0;
        while (isdigit((unsigned char)*p)) {
            unsigned d = (unsigned)(*p - '0');
            if (val > (1000000u - d) / 10u) return 0;
            val = val * 10u + d;
            p++;
        }
        parts[k] = val;
        if (k < 2) {
            if (*p != '.') return 0;
            p++;
        }
    }
    if (*p == '-') {
        size_t n = 0;
        p++;
        if (!*p || *p == '+') return 0;
        while (*p && *p != '+') {
            if (n + 1 >= sizeof(v->pre)) return 0;
            v->pre[n++] = *p++;
        }
        v->pre[n] = 0;
    }
    if (*p == '+') {
        p++;
        if (!*p) return 0;
        while (*p) p++;
    }
    if (*p != 0) return 0;
    v->major = parts[0]; v->minor = parts[1]; v->patch = parts[2];
    return 1;
}

/* Returns 1 if candidate is a valid semver strictly newer than current, 0 if up to date/older,
 * or -1 if candidate is not a valid semver tag. "dev" is treated as older than any valid release. */
static inline int lu_version_is_newer(const char *current, const char *candidate)
{
    struct lu_semver cur, cand;
    if (!lu_parse_semver(candidate, &cand)) return -1;
    if (!current || !strcmp(current, "dev")) return 1;
    if (!lu_parse_semver(current, &cur)) return -1;
    if (cand.major != cur.major) return cand.major > cur.major ? 1 : 0;
    if (cand.minor != cur.minor) return cand.minor > cur.minor ? 1 : 0;
    if (cand.patch != cur.patch) return cand.patch > cur.patch ? 1 : 0;
    if (cur.pre[0] && !cand.pre[0]) return 1;
    if (!cur.pre[0] && cand.pre[0]) return 0;
    return strcmp(cand.pre, cur.pre) > 0 ? 1 : 0;
}

/* ---- Check throttling & rate-limit policy ---- */

static inline int lu_can_check(const struct lu_schedule *s, uint64_t now, int manual)
{
    if (now < s->retry_after) return 0;
    if (manual) return 1;
    if (!s->auto_enabled) return 0;
    if (s->last_check && now < s->last_check + LU_INTERVAL_SEC) return 0;
    return 1;
}

static inline enum lu_result lu_http_classify(int status_code, int ratelimit_remaining,
                                              uint32_t retry_after_sec, uint64_t now,
                                              struct lu_schedule *s)
{
    if (status_code <= 0) return LU_ERR_OFFLINE;
    if (status_code == 429 || (status_code == 403 && ratelimit_remaining == 0)) {
        uint32_t wait_s = retry_after_sec ? retry_after_sec : 3600u;
        if (wait_s > 86400u) wait_s = 86400u;
        s->retry_after = now + wait_s;
        return LU_ERR_RATELIMIT;
    }
    if (status_code != 200) return LU_ERR_HTTP;
    s->last_check = now;
    return LU_OK;
}

/* ---- JSON release parser & asset selector ---- */

static inline const char *lu_json_str(const char *p, const char *end, char *out, size_t cap, int allow_newlines)
{
    size_t n = 0;
    if (p >= end || *p != '"') return NULL;
    p++;
    while (p < end && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            if (p >= end) return NULL;
            c = (unsigned char)*p++;
            if (c == 'n') c = allow_newlines ? '\n' : ' ';
            else if (c == 'r') { if (allow_newlines) continue; c = ' '; }
            else if (c == 't') c = ' ';
            else if (c == 'u') {
                if (end - p < 4) return NULL;
                p += 4;
                c = '?';
            }
        }
        if (c == '\n' && allow_newlines) {
            if (n + 1 < cap) out[n++] = '\n';
        } else if (c >= 32 && c <= 126) {
            if (n + 1 < cap) out[n++] = (char)c;
        }
    }
    if (p >= end || *p != '"') return NULL;
    out[n] = 0;
    return p + 1;
}

static inline const char *lu_json_find_key(const char *start, const char *end, const char *key)
{
    size_t klen = strlen(key);
    const char *p = start;
    while (p < end) {
        if (*p == '"') {
            char tmp[64];
            const char *after = lu_json_str(p, end, tmp, sizeof(tmp), 0);
            if (!after) return NULL;
            p = after;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p < end && *p == ':' && strlen(tmp) == klen && !strcmp(tmp, key)) {
                p++;
                while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
                return p;
            }
        } else {
            p++;
        }
    }
    return NULL;
}

static inline int lu_json_uint(const char *p, const char *end, size_t *out)
{
    size_t v = 0;
    if (p >= end || !isdigit((unsigned char)*p)) return 0;
    while (p < end && isdigit((unsigned char)*p)) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (LU_MAX_ASSET_BYTES - d) / 10u) return 0;
        v = v * 10u + d;
        p++;
    }
    *out = v;
    return 1;
}

static inline int lu_url_valid(const char *url)
{
    size_t i, n = strlen(url);
    if (n < 10 || n >= LU_URL_MAX) return 0;
    if (strncmp(url, "https://", 8) && strncmp(url, "http://127.0.0.1:", 17) && strncmp(url, "http://localhost:", 17))
        return 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)url[i] <= 32 || (unsigned char)url[i] > 126) return 0;
    return 1;
}

static inline enum lu_result lu_parse_release(const char *json, size_t len, const char *prefix, struct lu_release *out)
{
    const char *end = json + len, *v, *assets, *p;
    struct lu_semver sv;
    memset(out, 0, sizeof(*out));
    if (!json || !len || len >= LU_MAX_JSON_BYTES || memchr(json, 0, len)) return LU_ERR_JSON;
    v = lu_json_find_key(json, end, "tag_name");
    if (!v || !lu_json_str(v, end, out->tag, sizeof(out->tag), 0)) return LU_ERR_JSON;
    if (!lu_parse_semver(out->tag, &sv)) return LU_ERR_VERSION;
    v = lu_json_find_key(json, end, "body");
    if (v && *v == '"') lu_json_str(v, end, out->notes, sizeof(out->notes), 1);
    snprintf(out->zip_name, sizeof(out->zip_name), "%s-%s.zip", prefix, out->tag);
    snprintf(out->sha_name, sizeof(out->sha_name), "%s-%s.zip.sha256", prefix, out->tag);
    assets = lu_json_find_key(json, end, "assets");
    if (!assets || *assets != '[') return LU_ERR_ASSET;
    p = assets + 1;
    while (p < end && *p != ']') {
        if (*p == '{') {
            const char *obj_start = p + 1, *obj_end = p + 1;
            int depth = 1;
            while (obj_end < end && depth > 0) {
                if (*obj_end == '"') {
                    char skip[4];
                    const char *next = lu_json_str(obj_end, end, skip, sizeof(skip), 1);
                    if (!next) return LU_ERR_JSON;
                    obj_end = next;
                    continue;
                }
                if (*obj_end == '{') depth++;
                else if (*obj_end == '}') depth--;
                if (depth > 0) obj_end++;
            }
            if (depth != 0) return LU_ERR_JSON;
            {
                char name[112] = {0}, url[LU_URL_MAX] = {0};
                size_t sz = 0;
                const char *nk = lu_json_find_key(obj_start, obj_end, "name");
                const char *uk = lu_json_find_key(obj_start, obj_end, "browser_download_url");
                const char *sk = lu_json_find_key(obj_start, obj_end, "size");
                if (nk && uk && sk && lu_json_str(nk, obj_end, name, sizeof(name), 0) &&
                    lu_json_str(uk, obj_end, url, sizeof(url), 0) && lu_json_uint(sk, obj_end, &sz) && sz > 0 && lu_url_valid(url)) {
                    if (!strcmp(name, out->zip_name)) {
                        strcpy(out->zip_url, url);
                        out->zip_size = sz;
                    } else if (!strcmp(name, out->sha_name) && sz <= 4096u) {
                        strcpy(out->sha_url, url);
                        out->sha_size = sz;
                    }
                }
            }
            p = obj_end + 1;
        } else {
            p++;
        }
    }
    if (!out->zip_url[0] || !out->zip_size || !out->sha_url[0] || !out->sha_size)
        return LU_ERR_ASSET;
    return LU_OK;
}

/* ---- Sidecar & download verification ---- */

static inline enum lu_result lu_parse_sidecar(const char *text, size_t len, const char *expected_zip_name, char out_hash[65])
{
    size_t i = 0, name_start, name_len;
    if (!text || len < 66 || len > 4096 || memchr(text, 0, len)) return LU_ERR_SIDECAR;
    for (i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!isxdigit(c)) return LU_ERR_SIDECAR;
        out_hash[i] = (char)tolower(c);
    }
    out_hash[64] = 0;
    if (text[i] != ' ' && text[i] != '\t') return LU_ERR_SIDECAR;
    while (i < len && (text[i] == ' ' || text[i] == '\t')) i++;
    if (i < len && text[i] == '*') i++;
    name_start = i;
    while (i < len && text[i] != '\r' && text[i] != '\n' && text[i] != ' ' && text[i] != '\t') i++;
    name_len = i - name_start;
    while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) i++;
    if (i != len || name_len != strlen(expected_zip_name) || strncmp(text + name_start, expected_zip_name, name_len))
        return LU_ERR_SIDECAR;
    return LU_OK;
}

static inline enum lu_result lu_verify_download(const unsigned char *bytes, size_t actual_size,
                                                size_t declared_size, const char *expected_sha256)
{
    char actual_hash[65];
    if (!bytes || !declared_size || actual_size != declared_size || actual_size > LU_MAX_ASSET_BYTES)
        return LU_ERR_TRUNCATED;
    lu_sha256_hex(bytes, actual_size, actual_hash);
    if (strcmp(actual_hash, expected_sha256) != 0)
        return LU_ERR_CHECKSUM;
    return LU_OK;
}

/* ---- RFC 1951 Inflate + ZIP extractor + PE check ---- */

struct lu_bits {
    const unsigned char *src;
    size_t size, pos;
    uint32_t bitbuf;
    int bitcnt;
};

static inline int lu_getbits(struct lu_bits *b, int n, unsigned *out)
{
    while (b->bitcnt < n) {
        if (b->pos >= b->size) return 0;
        b->bitbuf |= (uint32_t)b->src[b->pos++] << b->bitcnt;
        b->bitcnt += 8;
    }
    *out = (unsigned)(b->bitbuf & ((1u << n) - 1u));
    b->bitbuf >>= n;
    b->bitcnt -= n;
    return 1;
}

struct lu_huff {
    uint16_t count[16];
    uint16_t symbol[288];
};

static inline int lu_huff_build(struct lu_huff *h, const unsigned char *lengths, int nsyms)
{
    uint16_t offs[16];
    int i, left = 1;
    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < nsyms; i++) {
        if (lengths[i] > 15) return 0;
        h->count[lengths[i]]++;
    }
    if (h->count[0] == nsyms) return 1;
    for (i = 1; i <= 15; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return 0;
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (i = 0; i < nsyms; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    return 1;
}

static inline int lu_huff_decode(struct lu_bits *b, const struct lu_huff *h, int *sym)
{
    int len, code = 0, first = 0, index = 0;
    for (len = 1; len <= 15; len++) {
        unsigned bit;
        if (!lu_getbits(b, 1, &bit)) return 0;
        code |= (int)bit;
        if (code - h->count[len] < first) {
            *sym = h->symbol[index + (code - first)];
            return 1;
        }
        index += h->count[len];
        first = (first + h->count[len]) << 1;
        code <<= 1;
    }
    return 0;
}

static inline int lu_inflate(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_cap, size_t *out_len)
{
    static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t lextra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t dextra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    static const uint8_t clorder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    struct lu_bits b = { src, src_len, 0, 0, 0 };
    size_t out = 0;
    unsigned bfinal = 0;
    do {
        unsigned btype;
        if (!lu_getbits(&b, 1, &bfinal) || !lu_getbits(&b, 2, &btype)) return 0;
        if (btype == 0) {
            uint16_t len, nlen;
            b.bitbuf = 0; b.bitcnt = 0;
            if (b.pos + 4 > b.size) return 0;
            len = (uint16_t)(b.src[b.pos] | (b.src[b.pos+1] << 8));
            nlen = (uint16_t)(b.src[b.pos+2] | (b.src[b.pos+3] << 8));
            b.pos += 4;
            if ((uint16_t)~nlen != len || b.pos + len > b.size || out + len > dst_cap) return 0;
            memcpy(dst + out, b.src + b.pos, len);
            b.pos += len; out += len;
        } else if (btype == 1 || btype == 2) {
            struct lu_huff lit_huff, dist_huff;
            unsigned char lengths[320];
            int i, nlit = 288, ndist = 32;
            if (btype == 1) {
                for (i = 0; i < 144; i++) lengths[i] = 8;
                for (; i < 256; i++) lengths[i] = 9;
                for (; i < 280; i++) lengths[i] = 7;
                for (; i < 288; i++) lengths[i] = 8;
                for (; i < 320; i++) lengths[i] = 5;
            } else {
                unsigned hlit, hdist, hclen;
                unsigned char clen[19] = {0};
                struct lu_huff chuff;
                if (!lu_getbits(&b, 5, &hlit) || !lu_getbits(&b, 5, &hdist) || !lu_getbits(&b, 4, &hclen)) return 0;
                nlit = (int)hlit + 257; ndist = (int)hdist + 1;
                if (nlit > 286 || ndist > 30) return 0;
                for (i = 0; i < (int)hclen + 4; i++) {
                    unsigned v;
                    if (!lu_getbits(&b, 3, &v)) return 0;
                    clen[clorder[i]] = (unsigned char)v;
                }
                if (!lu_huff_build(&chuff, clen, 19)) return 0;
                i = 0;
                while (i < nlit + ndist) {
                    int sym;
                    if (!lu_huff_decode(&b, &chuff, &sym)) return 0;
                    if (sym < 16) {
                        lengths[i++] = (unsigned char)sym;
                    } else {
                        unsigned rep = 0;
                        unsigned char prev = 0;
                        if (sym == 16) {
                            if (i == 0 || !lu_getbits(&b, 2, &rep)) return 0;
                            prev = lengths[i - 1]; rep += 3;
                        } else if (sym == 17) {
                            if (!lu_getbits(&b, 3, &rep)) return 0;
                            rep += 3;
                        } else if (sym == 18) {
                            if (!lu_getbits(&b, 7, &rep)) return 0;
                            rep += 11;
                        } else return 0;
                        if (i + (int)rep > nlit + ndist) return 0;
                        while (rep--) lengths[i++] = prev;
                    }
                }
            }
            if (!lu_huff_build(&lit_huff, lengths, nlit) || !lu_huff_build(&dist_huff, lengths + nlit, ndist)) return 0;
            for (;;) {
                int sym;
                if (!lu_huff_decode(&b, &lit_huff, &sym)) return 0;
                if (sym < 256) {
                    if (out >= dst_cap) return 0;
                    dst[out++] = (unsigned char)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    unsigned len, dist, eb;
                    int dsym;
                    sym -= 257;
                    if (sym >= 29 || !lu_getbits(&b, lextra[sym], &eb)) return 0;
                    len = lbase[sym] + eb;
                    if (!lu_huff_decode(&b, &dist_huff, &dsym) || dsym >= 30 || !lu_getbits(&b, dextra[dsym], &eb)) return 0;
                    dist = dbase[dsym] + eb;
                    if (dist > out || out + len > dst_cap) return 0;
                    while (len--) {
                        dst[out] = dst[out - dist];
                        out++;
                    }
                }
            }
        } else return 0;
    } while (!bfinal);
    *out_len = out;
    return 1;
}

static inline uint16_t lu_u16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t lu_u32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

static inline int lu_zip_path_safe(const char *path, size_t len)
{
    size_t i;
    if (!len || len >= 240 || path[0] == '/' || path[0] == '\\') return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 32 || c > 126 || c == '\\' || c == ':') return 0;
        if (c == '.' && i + 1 < len && path[i+1] == '.') return 0;
    }
    return 1;
}

static inline enum lu_result lu_zip_extract(const unsigned char *zip, size_t zip_len, const char *member_name,
                                            unsigned char **out_buf, size_t *out_len)
{
    size_t eocd = 0, cd_off, cd_size, i, pos;
    uint16_t cd_entries;
    *out_buf = NULL; *out_len = 0;
    if (!zip || zip_len < 22 || zip_len > LU_MAX_ASSET_BYTES) return LU_ERR_ARCHIVE;
    for (i = zip_len - 22; ; i--) {
        if (zip[i] == 'P' && zip[i+1] == 'K' && zip[i+2] == 5 && zip[i+3] == 6) {
            eocd = i; break;
        }
        if (i == 0 || zip_len - i > 65557u) return LU_ERR_ARCHIVE;
    }
    cd_entries = lu_u16(zip + eocd + 10);
    cd_size = lu_u32(zip + eocd + 12);
    cd_off = lu_u32(zip + eocd + 16);
    if (cd_off + cd_size > eocd) return LU_ERR_ARCHIVE;
    pos = cd_off;
    for (i = 0; i < cd_entries; i++) {
        uint16_t method, nlen, elen, clen;
        uint32_t crc, comp_sz, uncomp_sz, lhoff;
        const char *name;
        if (pos + 46 > cd_off + cd_size || memcmp(zip + pos, "PK\x01\x02", 4)) return LU_ERR_ARCHIVE;
        method = lu_u16(zip + pos + 10);
        crc = lu_u32(zip + pos + 16);
        comp_sz = lu_u32(zip + pos + 20);
        uncomp_sz = lu_u32(zip + pos + 24);
        nlen = lu_u16(zip + pos + 28);
        elen = lu_u16(zip + pos + 30);
        clen = lu_u16(zip + pos + 32);
        lhoff = lu_u32(zip + pos + 42);
        if (pos + 46 + nlen + elen + clen > cd_off + cd_size) return LU_ERR_ARCHIVE;
        name = (const char *)(zip + pos + 46);
        if (!lu_zip_path_safe(name, nlen)) return LU_ERR_ARCHIVE;
        if (nlen == strlen(member_name) && !memcmp(name, member_name, nlen)) {
            uint16_t lnlen, lelen;
            size_t data_off, actual_uncomp = 0;
            unsigned char *buf;
            if (!uncomp_sz || uncomp_sz > LU_MAX_ASSET_BYTES || lhoff + 30 > cd_off) return LU_ERR_ARCHIVE;
            if (memcmp(zip + lhoff, "PK\x03\x04", 4)) return LU_ERR_ARCHIVE;
            lnlen = lu_u16(zip + lhoff + 26);
            lelen = lu_u16(zip + lhoff + 28);
            data_off = lhoff + 30u + lnlen + lelen;
            if (data_off + comp_sz > cd_off) return LU_ERR_ARCHIVE;
            buf = (unsigned char *)malloc(uncomp_sz + 1);
            if (!buf) return LU_ERR_IO;
            if (method == 0) {
                if (comp_sz != uncomp_sz) { free(buf); return LU_ERR_ARCHIVE; }
                memcpy(buf, zip + data_off, uncomp_sz);
                actual_uncomp = uncomp_sz;
            } else if (method == 8) {
                if (!lu_inflate(zip + data_off, comp_sz, buf, uncomp_sz, &actual_uncomp) || actual_uncomp != uncomp_sz) {
                    free(buf); return LU_ERR_ARCHIVE;
                }
            } else {
                free(buf); return LU_ERR_ARCHIVE;
            }
            if (lu_crc32(buf, uncomp_sz) != crc) {
                free(buf); return LU_ERR_ARCHIVE;
            }
            buf[uncomp_sz] = 0;
            *out_buf = buf; *out_len = uncomp_sz;
            return LU_OK;
        }
        pos += 46u + nlen + elen + clen;
    }
    return LU_ERR_ARCHIVE;
}

static inline int lu_valid_pe32(const unsigned char *bytes, size_t len)
{
    uint32_t pe_off;
    if (!bytes || len < 256 || bytes[0] != 'M' || bytes[1] != 'Z') return 0;
    pe_off = lu_u32(bytes + 60);
    if (pe_off < 64 || pe_off + 24 > len) return 0;
    if (memcmp(bytes + pe_off, "PE\0\0", 4) != 0) return 0;
    if (lu_u16(bytes + pe_off + 4) != 0x014cu) return 0;
    return 1;
}

/* ---- Transactional file helpers, self-update and rollback ---- */

static inline int lu_read_bytes(const char *path, unsigned char **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    unsigned char *buf;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || (size_t)sz > LU_MAX_ASSET_BYTES || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return 0;
    }
    buf = (unsigned char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return 0;
    }
    fclose(f);
    buf[sz] = 0;
    *out_buf = buf; *out_len = (size_t)sz;
    return 1;
}

static inline int lu_file_sha256(const char *path, char out_hash[65])
{
    unsigned char *buf = NULL;
    size_t len = 0;
    if (!lu_read_bytes(path, &buf, &len)) return 0;
    lu_sha256_hex(buf, len, out_hash);
    free(buf);
    return 1;
}

static inline int lu_write_verified(const char *path, const unsigned char *bytes, size_t len, const char expected_hash[65])
{
    FILE *f = fopen(path, "wb");
    char check[65];
    if (!f) return 0;
    if (len > 0 && fwrite(bytes, 1, len, f) != len) { fclose(f); remove(path); return 0; }
    if (fclose(f) != 0) { remove(path); return 0; }
    if (!lu_file_sha256(path, check) || strcmp(check, expected_hash) != 0) {
        remove(path); return 0;
    }
    return 1;
}

static inline int lu_copy_verified(const char *src, const char *dst, char out_hash[65])
{
    unsigned char *buf = NULL;
    size_t len = 0;
    int ok;
    if (!lu_read_bytes(src, &buf, &len)) return 0;
    lu_sha256_hex(buf, len, out_hash);
    ok = lu_write_verified(dst, buf, len, out_hash);
    free(buf);
    return ok;
}

/* If an installer ownership receipt exists alongside target, update its installed hash
 * while preserving original so setup status stays 'current' after self-update. */
static inline void lu_sync_receipt(const char *target, const char new_hash[65])
{
    char rpath[512], tpath[512], text[1024], orig[65] = {0};
    unsigned char *buf = NULL;
    size_t len = 0;
    const char *p;
    FILE *f;
    snprintf(rpath, sizeof(rpath), "%s.coop-receipt.json", target);
    if (!lu_read_bytes(rpath, &buf, &len)) return;
    p = strstr((const char *)buf, "\"original\":");
    if (p) {
        p = strchr(p + 11, '"');
        if (p && strlen(p + 1) >= 64) {
            size_t i, hex_ok = 1;
            for (i = 0; i < 64; i++) if (!isxdigit((unsigned char)p[1 + i])) hex_ok = 0;
            if (hex_ok && p[65] == '"') memcpy(orig, p + 1, 64);
        }
    }
    free(buf);
    snprintf(tpath, sizeof(tpath), "%s.coop-receipt.json.tmp", target);
    if (orig[0])
        snprintf(text, sizeof(text), "{\"schema\":1,\"original\":\"%s\",\"installed\":\"%s\",\"pending\":null}", orig, new_hash);
    else
        snprintf(text, sizeof(text), "{\"schema\":1,\"original\":null,\"installed\":\"%s\",\"pending\":null}", new_hash);
    f = fopen(tpath, "wb");
    if (!f) return;
    if (fwrite(text, 1, strlen(text), f) == strlen(text) && fclose(f) == 0)
        lu_rename_replace(tpath, rpath);
    else {
        fclose(f);
        remove(tpath);
    }
}

static inline enum lu_result lu_rollback(const char *game_dir)
{
    char state_path[512], dll_path[512], lua_path[512], dll_prev[512], lua_prev[512], dll_stage[512], lua_stage[512];
    char prev_dll_hash[65] = {0}, prev_lua_hash[65] = {0}, check_dll[65], check_lua[65], prev_ver[LU_TAG_MAX] = {0};
    unsigned char *state_buf = NULL;
    size_t state_len = 0;
    const char *p;
    snprintf(state_path, sizeof(state_path), "%s/coop/update-state.ini", game_dir);
    snprintf(dll_path, sizeof(dll_path), "%s/dinput8.dll", game_dir);
    snprintf(lua_path, sizeof(lua_path), "%s/coop/coop.lua", game_dir);
    snprintf(dll_prev, sizeof(dll_prev), "%s/dinput8.dll.previous", game_dir);
    snprintf(lua_prev, sizeof(lua_prev), "%s/coop/coop.lua.previous", game_dir);
    snprintf(dll_stage, sizeof(dll_stage), "%s/dinput8.dll.rollback-stage", game_dir);
    snprintf(lua_stage, sizeof(lua_stage), "%s/coop/coop.lua.rollback-stage", game_dir);

    if (!lu_read_bytes(state_path, &state_buf, &state_len)) return LU_ERR_ROLLBACK;
    if ((p = strstr((const char *)state_buf, "prev_dll_sha256=")) && strlen(p + 16) >= 64) memcpy(prev_dll_hash, p + 16, 64);
    if ((p = strstr((const char *)state_buf, "prev_lua_sha256=")) && strlen(p + 16) >= 64) memcpy(prev_lua_hash, p + 16, 64);
    if ((p = strstr((const char *)state_buf, "previous_version="))) {
        size_t i = 0;
        p += 17;
        while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < sizeof(prev_ver)) { prev_ver[i] = p[i]; i++; }
        prev_ver[i] = 0;
    }
    free(state_buf);
    if (strlen(prev_dll_hash) != 64 || strlen(prev_lua_hash) != 64) return LU_ERR_ROLLBACK;
    if (!lu_file_sha256(dll_prev, check_dll) || strcmp(check_dll, prev_dll_hash) != 0) return LU_ERR_ROLLBACK;
    if (!lu_file_sha256(lua_prev, check_lua) || strcmp(check_lua, prev_lua_hash) != 0) return LU_ERR_ROLLBACK;
    if (!lu_copy_verified(dll_prev, dll_stage, check_dll) || !lu_copy_verified(lua_prev, lua_stage, check_lua)) {
        remove(dll_stage); remove(lua_stage); return LU_ERR_ROLLBACK;
    }
    if (!lu_rename_replace(dll_stage, dll_path) || !lu_rename_replace(lua_stage, lua_path)) {
        remove(dll_stage); remove(lua_stage); return LU_ERR_ROLLBACK;
    }
    if (!lu_file_sha256(dll_path, check_dll) || strcmp(check_dll, prev_dll_hash) != 0 ||
        !lu_file_sha256(lua_path, check_lua) || strcmp(check_lua, prev_lua_hash) != 0)
        return LU_ERR_ROLLBACK;
    lu_sync_receipt(dll_path, prev_dll_hash);
    lu_sync_receipt(lua_path, prev_lua_hash);
    remove(dll_prev);
    remove(lua_prev);
    remove(state_path);
    return LU_OK;
}

/* Self-update mod files and launcher DLL from a verified release ZIP archive.
 * Keeps .previous backups and update-state.ini; rolls back automatically if any step fails. */
static inline enum lu_result lu_apply_package(const char *game_dir, const unsigned char *zip_bytes, size_t zip_len,
                                              const char *prefix, const char *current_ver, const char *target_tag,
                                              int fail_after_dll)
{
    char dll_member[160], lua_member[160];
    char dll_path[512], lua_path[512], dll_stage[512], lua_stage[512], dll_prev[512], lua_prev[512], state_path[512];
    char new_dll_hash[65], new_lua_hash[65], prev_dll_hash[65], prev_lua_hash[65], check[65];
    unsigned char *dll_buf = NULL, *lua_buf = NULL;
    size_t dll_len = 0, lua_len = 0;
    enum lu_result r;
    FILE *sf;

    if (!lu_tag_safe(target_tag)) return LU_ERR_VERSION;
    snprintf(dll_member, sizeof(dll_member), "%s-%s/dinput8.dll", prefix, target_tag);
    snprintf(lua_member, sizeof(lua_member), "%s-%s/coop/coop.lua", prefix, target_tag);

    r = lu_zip_extract(zip_bytes, zip_len, dll_member, &dll_buf, &dll_len);
    if (r != LU_OK) return r;
    if (!lu_valid_pe32(dll_buf, dll_len)) { free(dll_buf); return LU_ERR_PE; }
    r = lu_zip_extract(zip_bytes, zip_len, lua_member, &lua_buf, &lua_len);
    if (r != LU_OK) { free(dll_buf); return r; }

    lu_sha256_hex(dll_buf, dll_len, new_dll_hash);
    lu_sha256_hex(lua_buf, lua_len, new_lua_hash);

    snprintf(dll_path, sizeof(dll_path), "%s/dinput8.dll", game_dir);
    snprintf(lua_path, sizeof(lua_path), "%s/coop/coop.lua", game_dir);
    snprintf(dll_stage, sizeof(dll_stage), "%s/dinput8.dll.update-stage", game_dir);
    snprintf(lua_stage, sizeof(lua_stage), "%s/coop/coop.lua.update-stage", game_dir);
    snprintf(dll_prev, sizeof(dll_prev), "%s/dinput8.dll.previous", game_dir);
    snprintf(lua_prev, sizeof(lua_prev), "%s/coop/coop.lua.previous", game_dir);
    snprintf(state_path, sizeof(state_path), "%s/coop/update-state.ini", game_dir);

    if (!lu_write_verified(dll_stage, dll_buf, dll_len, new_dll_hash) ||
        !lu_write_verified(lua_stage, lua_buf, lua_len, new_lua_hash)) {
        free(dll_buf); free(lua_buf);
        remove(dll_stage); remove(lua_stage);
        return LU_ERR_IO;
    }
    free(dll_buf); free(lua_buf);

    if (!lu_copy_verified(dll_path, dll_prev, prev_dll_hash) ||
        !lu_copy_verified(lua_path, lua_prev, prev_lua_hash)) {
        remove(dll_stage); remove(lua_stage);
        return LU_ERR_IO;
    }

    sf = fopen(state_path, "wb");
    if (!sf) {
        remove(dll_stage); remove(lua_stage);
        return LU_ERR_IO;
    }
    fprintf(sf, "[update]\nstatus=pending\nprevious_version=%s\ntarget_version=%s\nprev_dll_sha256=%s\nprev_lua_sha256=%s\nnew_dll_sha256=%s\nnew_lua_sha256=%s\n",
            current_ver ? current_ver : "dev", target_tag, prev_dll_hash, prev_lua_hash, new_dll_hash, new_lua_hash);
    fclose(sf);

    /* Rename the running DLL out of the way (supported on Windows for mapped DLLs), then move stage into place. */
    if (!lu_rename_replace(dll_path, dll_prev) || !lu_rename_replace(dll_stage, dll_path)) {
        remove(dll_stage); remove(lua_stage);
        lu_rollback(game_dir);
        return LU_ERR_IO;
    }
    if (fail_after_dll || !lu_rename_replace(lua_stage, lua_path) ||
        !lu_file_sha256(dll_path, check) || strcmp(check, new_dll_hash) != 0 ||
        !lu_file_sha256(lua_path, check) || strcmp(check, new_lua_hash) != 0) {
        remove(lua_stage);
        lu_rollback(game_dir);
        return LU_ERR_IO;
    }

    sf = fopen(state_path, "wb");
    if (sf) {
        fprintf(sf, "[update]\nstatus=applied\nprevious_version=%s\ntarget_version=%s\nprev_dll_sha256=%s\nprev_lua_sha256=%s\nnew_dll_sha256=%s\nnew_lua_sha256=%s\n",
                current_ver ? current_ver : "dev", target_tag, prev_dll_hash, prev_lua_hash, new_dll_hash, new_lua_hash);
        fclose(sf);
    }
    lu_sync_receipt(dll_path, new_dll_hash);
    lu_sync_receipt(lua_path, new_lua_hash);
    return LU_OK;
}

/* Startup recovery check: if a previous self-update was interrupted while status=pending,
 * automatically roll back to the verified .previous files. */
static inline int lu_recover_interrupted(const char *game_dir)
{
    char state_path[512];
    unsigned char *buf = NULL;
    size_t len = 0;
    int pending = 0;
    snprintf(state_path, sizeof(state_path), "%s/coop/update-state.ini", game_dir);
    if (!lu_read_bytes(state_path, &buf, &len)) return 0;
    pending = strstr((const char *)buf, "status=pending") != NULL;
    free(buf);
    if (!pending) return 0;
    return lu_rollback(game_dir) == LU_OK ? 1 : -1;
}

#endif
