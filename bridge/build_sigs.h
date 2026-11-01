/* The game builds the mod knows, shared by the DLL (dinput8.c select_build) and the installer helper
 * (dist/install-policy.c). A build is recognised by one fact: the builtin table entry of "copytagmethods"
 * points at that very string. The DLL checks it in the running process; the installer reads the same two
 * places from the exe on disk (cb_identify), without running the game. Pure C, no platform calls. */
#ifndef COOP_BUILD_SIGS_H
#define COOP_BUILD_SIGS_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* name, builtin_entry, builtin_name: the first three fields of a struct build_addrs initialiser */
#define COOP_BUILD_STEAM    "Steam build 3902", 0x506520, 0x4d7d34
#define COOP_BUILD_OVERHAUL "Overhaul Games build", 0x505510, 0x4d769c

struct cb_sig { const char *name; uint32_t entry, name_va; };
static const struct cb_sig CB_SIGS[] = { { COOP_BUILD_STEAM }, { COOP_BUILD_OVERHAUL } };
#define CB_COUNT ((int)(sizeof(CB_SIGS) / sizeof(CB_SIGS[0])))
#define CB_UNKNOWN (-1)

static inline uint32_t cb_u16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static inline uint32_t cb_u32(const unsigned char *p) { return cb_u16(p) | (cb_u16(p + 2) << 16); }

/* file offset of virtual address `va` (inside raw section data), or -1 */
static inline long cb_va_to_offset(const unsigned char *f, size_t len, uint32_t va)
{
    uint32_t pe, base, rva, nsec, optsize, i;
    if (len < 0x100 || f[0] != 'M' || f[1] != 'Z') return -1;
    pe = cb_u32(f + 0x3c);
    if (pe > len - 0x80 || memcmp(f + pe, "PE\0\0", 4) != 0) return -1;
    nsec = cb_u16(f + pe + 6);
    optsize = cb_u16(f + pe + 20);
    if (optsize < 0x60 || cb_u16(f + pe + 24) != 0x10b) return -1;      /* 32-bit image only */
    base = cb_u32(f + pe + 24 + 28);
    if (va < base) return -1;
    rva = va - base;
    if (nsec == 0 || nsec > 96 || (size_t)pe + 24 + optsize + (size_t)nsec * 40 > len) return -1;
    for (i = 0; i < nsec; i++) {
        const unsigned char *s = f + pe + 24 + optsize + i * 40;
        uint32_t vsize = cb_u32(s + 8), vaddr = cb_u32(s + 12), rawsize = cb_u32(s + 16), raw = cb_u32(s + 20);
        uint32_t span = vsize < rawsize ? vsize : rawsize;
        if (rva >= vaddr && rva - vaddr < span && (size_t)raw + (rva - vaddr) < len)
            return (long)(raw + (rva - vaddr));
    }
    return -1;
}

/* index into CB_SIGS of the build this exe image is, or CB_UNKNOWN. Same test as the DLL's select_build. */
static inline int cb_identify(const unsigned char *f, size_t len)
{
    int i;
    for (i = 0; i < CB_COUNT; i++) {
        long e = cb_va_to_offset(f, len, CB_SIGS[i].entry), n = cb_va_to_offset(f, len, CB_SIGS[i].name_va);
        if (e < 0 || n < 0 || (size_t)e + 4 > len || (size_t)n + 15 > len) continue;
        if (cb_u32(f + e) == CB_SIGS[i].name_va && memcmp(f + n, "copytagmethods", 15) == 0) return i;
    }
    return CB_UNKNOWN;
}
#endif
