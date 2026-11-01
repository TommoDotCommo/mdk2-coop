/* Portable decisions of the one-file Setup (dist/installer.c): the payload container, the names inside it and the
 * "run from a zip" test. No filesystem or platform calls, so tools/test_setup_core.py runs it (and its mutants) natively.
 *
 * Payload container, little endian: "MCP1", u32 count, then per file: u16 name length, name (ASCII path with '/'),
 * u32 size, bytes. The whole blob is the Setup exe's RCDATA resource 10; resource 12 holds its SHA-256 in hex. */
#ifndef COOP_SETUP_CORE_H
#define COOP_SETUP_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#define SC_MAX_FILES 64
#define SC_MAX_FILE  (32u * 1024u * 1024u)

struct sc_file { char name[96]; const unsigned char *data; uint32_t size; };

static inline uint32_t sc_u16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static inline uint32_t sc_u32(const unsigned char *p) { return sc_u16(p) | (sc_u16(p + 2) << 16); }

/* A payload name must stay inside the target folder: relative, plain characters, no empty or dot components. */
static inline int sc_name_ok(const char *name, size_t len)
{
    size_t i, start = 0;
    if (!len || len >= 96) return 0;
    for (i = 0; i <= len; i++) {
        if (i == len || name[i] == '/') {
            size_t n = i - start;
            if (!n || name[start + n - 1] == '.') return 0;   /* empty, ".", "..", or a trailing dot Windows would drop */
            start = i + 1;
            continue;
        }
        {
            unsigned char c = (unsigned char)name[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                return 0;       /* no backslash, colon, space, control or non-ASCII characters */
        }
    }
    return 1;
}

static inline int sc_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* Parse the container. Returns the file count (1..SC_MAX_FILES) or -1 for anything that is not exactly well formed:
 * bad magic, names that escape or repeat (Windows names compare case-insensitively), sizes past the blob, trailing bytes. */
static inline int sc_parse(const unsigned char *blob, size_t len, struct sc_file *out)
{
    size_t pos = 8;
    uint32_t count, i, j;
    if (!blob || len < 8 || memcmp(blob, "MCP1", 4) != 0) return -1;
    count = sc_u32(blob + 4);
    if (count < 1 || count > SC_MAX_FILES) return -1;
    for (i = 0; i < count; i++) {
        uint32_t nlen, size;
        if (pos + 2 > len) return -1;
        nlen = sc_u16(blob + pos);
        pos += 2;
        if (!nlen || nlen >= sizeof(out[i].name) || pos + nlen + 4 > len) return -1;
        if (!sc_name_ok((const char *)blob + pos, nlen)) return -1;
        memcpy(out[i].name, blob + pos, nlen);
        out[i].name[nlen] = 0;
        for (j = 0; j < i; j++) if (sc_ieq(out[j].name, out[i].name)) return -1;
        pos += nlen;
        size = sc_u32(blob + pos);
        pos += 4;
        if (size > SC_MAX_FILE || pos + size > len) return -1;
        out[i].data = blob + pos;
        out[i].size = size;
        pos += size;
    }
    return pos == len ? (int)count : -1;
}

static inline wchar_t sc_fold(wchar_t c) { return c >= L'A' && c <= L'Z' ? (wchar_t)(c + 32) : c; }

/* The Setup was started from inside a zip that Explorer opened, not from an extracted folder: the path lies under the temp
 * folder and one of its components ends in ".zip" (Explorer extracts a single file to %TEMP%\Temp1_<name>.zip\...).
 * `temp` may end in a backslash; both strings use '\\' separators. */
static inline int sc_run_from_zip(const wchar_t *exe, const wchar_t *temp)
{
    size_t tlen = wcslen(temp), i, start;
    while (tlen && (temp[tlen - 1] == L'\\' || temp[tlen - 1] == L'/')) tlen--;
    if (!tlen || wcslen(exe) <= tlen + 1) return 0;
    for (i = 0; i < tlen; i++) {
        wchar_t a = sc_fold(exe[i]), b = sc_fold(temp[i]);
        if ((a == L'/' ? L'\\' : a) != (b == L'/' ? L'\\' : b)) return 0;
    }
    if (exe[tlen] != L'\\' && exe[tlen] != L'/') return 0;
    for (start = tlen + 1, i = start; exe[i]; i++) {
        if (exe[i + 1] == 0 || exe[i + 1] == L'\\' || exe[i + 1] == L'/') {
            size_t end = i + 1;     /* component is exe[start..end), the last one is the exe itself */
            if (exe[i + 1] != 0 && end - start >= 4 && sc_fold(exe[end - 4]) == L'.' && sc_fold(exe[end - 3]) == L'z' &&
                sc_fold(exe[end - 2]) == L'i' && sc_fold(exe[end - 1]) == L'p') return 1;
            start = end + 1;
        }
    }
    return 0;
}
#endif
