/*
 * Gob helpers and code patching.
 */
#include <stdio.h>
#include <string.h>
#include "bridge.h"

const char *gob_name(BYTE *gob)
{
    static char buf[32];
    int i;

    if (!gob || IsBadReadPtr(gob + GOB_NAME, sizeof(buf)))
        return "?";
    for (i = 0; i < (int)sizeof(buf) - 1 && gob[GOB_NAME + i]; i++)
        buf[i] = (gob[GOB_NAME + i] >= 32 && gob[GOB_NAME + i] < 127) ? gob[GOB_NAME + i] : '.';
    buf[i] = 0;
    return buf;
}

int gob_type(BYTE *gob)
{
    BYTE *info = *(BYTE **)(gob + GOB_INFO);
    return info ? *(short *)info : 0;
}

int gob_class(BYTE *gob)
{
    BYTE *info = *(BYTE **)(gob + GOB_INFO);
    return info ? *(DWORD *)(info + INFO_CLASS) & 0x3f : 0;
}

int gob_hp(BYTE *gob)
{
    BYTE *info;

    if (!gob || IsBadReadPtr(gob + GOB_INFO, 4) || !(info = *(BYTE **)(gob + GOB_INFO)) || IsBadReadPtr(info + 0x12, 2))
        return -1;
    return *(short *)(info + 0x12);
}

int gob_is_actor(BYTE *gob)
{
    int t;

    if (!*(BYTE **)(gob + GOB_INFO))
        return 0;
    t = gob_type(gob);
    return t >= ACTOR_TYPE_MIN && t <= ACTOR_TYPE_MAX;
}

int gob_is_projectile(BYTE *gob)
{
    int t;

    if (!gob || IsBadReadPtr(gob + GOB_INFO, 4) || !*(BYTE **)(gob + GOB_INFO))
        return 0;
    t = gob_type(gob);
    return t >= PROJECTILE_TYPE_MIN && t <= PROJECTILE_TYPE_MAX;
}

int gob_slot(BYTE *gob)
{
    int i;

    for (i = 0; i < GOB_SLOTS; i++)
        if ((BYTE *)GOB_TABLE[i * 2] == gob)
            return i;
    return -1;
}

unsigned int gob_handle(int slot)
{
    return ((GOB_TABLE[slot * 2 + 1] & 0xffff) << 16) | (unsigned int)slot;
}

BYTE *gob_by_handle(unsigned int handle)
{
    unsigned int slot = handle & 0xffff;

    if (slot >= GOB_SLOTS || (GOB_TABLE[slot * 2 + 1] & 0xffff) != handle >> 16)
        return NULL;
    return (BYTE *)GOB_TABLE[slot * 2];
}

/* Name the peer knows the gob by: its own name, or "n<host handle>" for an unnamed actor
 * (the client creates its copy under that name) */
void gob_netname(BYTE *gob, char *out, int size)
{
    int slot;

    snprintf(out, size, "%s", gob_name(gob));
    if (out[0] || !gob || !(gob_is_actor(gob) || gob_is_projectile(gob)))
        return;
    if (!role_host) {
        out[0] = 0;
        return;
    }
    slot = gob_slot(gob);
    if (slot >= 0)
        snprintf(out, size, "n%x", gob_handle(slot));
}

void push_gob(BYTE *gob)
{
    if (gob)
        lua_pushusertag(gob, tolua_tag("OMGOB"));
    else
        lua_pushnil();
}

/* ---- mirrored gobs: drawn from the host's snapshots, their own think skipped ---- */

#define GOB_SET 1024        /* open addressing, power of two */

static BYTE *mirror_set[GOB_SET];      /* drawn from host snapshots, think skipped */
static BYTE *foreign_set[GOB_SET];     /* host shots replayed here: fly normally, deal no damage */
static DWORD think_orig[64];

static unsigned int set_hash(BYTE *gob)
{
    return ((DWORD)gob >> 4) & (GOB_SET - 1);
}

static int set_has(BYTE **set, BYTE *gob)
{
    unsigned int i;

    for (i = set_hash(gob); set[i]; i = (i + 1) & (GOB_SET - 1))
        if (set[i] == gob)
            return 1;
    return 0;
}

static void set_put(BYTE **set, BYTE *gob, int on)
{
    unsigned int i, j;

    for (i = set_hash(gob); set[i] && set[i] != gob; i = (i + 1) & (GOB_SET - 1))
        ;
    if (on) {
        set[i] = gob;
        return;
    }
    if (!set[i])
        return;
    /* delete and re-insert the rest of the cluster */
    set[i] = NULL;
    for (j = (i + 1) & (GOB_SET - 1); set[j]; j = (j + 1) & (GOB_SET - 1)) {
        BYTE *g = set[j];
        set[j] = NULL;
        set_put(set, g, 1);
    }
}

int gob_is_mirrored(BYTE *gob)
{
    return set_has(mirror_set, gob);
}

void gob_set_mirrored(BYTE *gob, int on)
{
    set_put(mirror_set, gob, on);
}

int gob_is_foreign(BYTE *gob)
{
    return set_has(foreign_set, gob);
}

void gob_set_foreign(BYTE *gob, int on)
{
    set_put(foreign_set, gob, on);
}

/* Called by the think thunk: the original think for normal gobs, 0 to skip mirrors */
static DWORD __cdecl think_filter(BYTE *gob)
{
    if (gob_is_mirrored(gob))
        return 0;
    return think_orig[gob_class(gob)];
}

/* Route a class's think slot through think_filter so mirrored gobs skip their AI.
 * Thunk: push [esp+4]; call think_filter; add esp,4; test eax,eax; jz +2; jmp eax;
 *        xor eax,eax; ret */
void class_filter_think(int cls)
{
    static BYTE *thunk;
    DWORD *desc, old;

    if (cls <= 0 || cls >= 64 || think_orig[cls])
        return;
    desc = (DWORD *)CLASS_TABLE[cls];
    if (!desc[CLASS_THINK])
        return;
    if (!thunk) {
        static const BYTE code[21] = {
            0xff, 0x74, 0x24, 0x04,         /* push dword [esp+4] */
            0xe8, 0, 0, 0, 0,               /* call think_filter */
            0x83, 0xc4, 0x04,               /* add esp, 4 */
            0x85, 0xc0,                     /* test eax, eax */
            0x74, 0x02,                     /* jz skip */
            0xff, 0xe0,                     /* jmp eax */
            0x31, 0xc0,                     /* skip: xor eax, eax */
            0xc3,                           /* ret */
        };
        thunk = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!thunk)
            return;
        memcpy(thunk, code, sizeof(code));
        *(DWORD *)(thunk + 5) = (DWORD)think_filter - (DWORD)(thunk + 9);
    }
    /* classes can share a descriptor: give them all the original */
    {
        DWORD orig = desc[CLASS_THINK];
        int k;
        for (k = 1; k < 64; k++)
            if (CLASS_TABLE[k] == (BYTE *)desc)
                think_orig[k] = orig;
    }
    VirtualProtect(desc, CLASS_SLOTS * 4, PAGE_READWRITE, &old);
    desc[CLASS_THINK] = (DWORD)thunk;
    VirtualProtect(desc, CLASS_SLOTS * 4, old, &old);
    logmsg("class %d: think %lx filtered (mirrored gobs skip it)", cls, (unsigned long)think_orig[cls]);
}

/* Redirect fn to hook with a jmp over its first len bytes (whole instructions with no
 * relative addressing); returns a trampoline that runs them and jumps back, or NULL if
 * the bytes are not what we expect. */
void *hook_code(BYTE *fn, const BYTE *expect, int len, void *hook)
{
    BYTE *tramp;
    DWORD old;

    if (len < 5 || memcmp(fn, expect, len) != 0)
        return NULL;
    tramp = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return NULL;
    memcpy(tramp, fn, len);
    tramp[len] = 0xe9;
    *(DWORD *)(tramp + len + 1) = (DWORD)(fn + len) - (DWORD)(tramp + len + 5);
    VirtualProtect(fn, len, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xe9;
    *(DWORD *)(fn + 1) = (DWORD)hook - (DWORD)(fn + 5);
    memset(fn + 5, 0x90, len - 5);
    VirtualProtect(fn, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, len);
    return tramp;
}
