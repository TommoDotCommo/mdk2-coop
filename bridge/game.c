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
    if (out[0] || !gob || !gob_is_actor(gob))
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

/* Client: stop a class's per-frame AI by pointing its think slot at "xor eax,eax; ret" */
void class_skip_think(int cls)
{
    static const BYTE stub[3] = { 0x31, 0xc0, 0xc3 };
    static BYTE *stub_code;
    static int skipped[64];
    DWORD *desc, old;

    if (cls <= 0 || cls >= 64 || skipped[cls])
        return;
    if (!stub_code && (stub_code = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
        memcpy(stub_code, stub, sizeof(stub));
    if (!stub_code)
        return;
    desc = (DWORD *)CLASS_TABLE[cls];
    if (!desc[CLASS_THINK])
        return;
    VirtualProtect(desc, CLASS_SLOTS * 4, PAGE_READWRITE, &old);
    logmsg("class %d: think %lx skipped (host drives it)", cls, (unsigned long)desc[CLASS_THINK]);
    desc[CLASS_THINK] = (DWORD)stub_code;
    VirtualProtect(desc, CLASS_SLOTS * 4, old, &old);
    skipped[cls] = 1;
}

/* Redirect fn to hook with a 5-byte jmp; returns a trampoline that runs the
 * saved prologue and jumps back, or NULL if the bytes are not what we expect. */
void *hook5(BYTE *fn, const BYTE *expect, void *hook)
{
    BYTE *tramp;
    DWORD old;

    if (memcmp(fn, expect, 5) != 0)
        return NULL;
    tramp = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return NULL;
    memcpy(tramp, fn, 5);
    tramp[5] = 0xe9;
    *(DWORD *)(tramp + 6) = (DWORD)(fn + 5) - (DWORD)(tramp + 10);
    VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xe9;
    *(DWORD *)(fn + 1) = (DWORD)hook - (DWORD)(fn + 5);
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    return tramp;
}
