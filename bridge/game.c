/*
 * Gob helpers and code patching.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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

static int name_unique(BYTE *gob, const char *name)
{
    int i;

    /* gobs without object info don't count: a spawned soldier's (l8r5_sod01_spawn, type 240)
     * two parts carry its name, and the client would then make a bare copy with 0 hp */
    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *g = (BYTE *)GOB_TABLE[i * 2];
        if (g && g != gob && *(BYTE **)(g + GOB_INFO) && strncmp((const char *)g + GOB_NAME, name, 32) == 0)
            return 0;
    }
    return 1;
}

/* gob_delete only marks a gob: it stays in the table (and its Lua global keeps working)
 * until the scene's cleanup, so a name lookup in the same frame would still find it.
 * gob_kill remembers what it deleted until the slot holds something else. */
#define MAX_DOOMED 32
static struct { BYTE *gob; unsigned int handle; } doomed[MAX_DOOMED];
static int ndoomed;         /* deletions so far; doomed[] holds the last MAX_DOOMED */

int gob_doomed(BYTE *gob)
{
    int i;

    for (i = 0; i < ndoomed && i < MAX_DOOMED; i++)
        if (doomed[i].gob == gob && gob_by_handle(doomed[i].handle) == gob)
            return 1;
    return 0;
}

void gob_kill(BYTE *gob)
{
    int slot = gob_slot(gob);

    if (slot < 0)
        return;
    doomed[ndoomed % MAX_DOOMED].gob = gob;
    doomed[ndoomed % MAX_DOOMED].handle = gob_handle(slot);
    ndoomed++;
    gob_delete(gob);
}

int gob_alive(BYTE *gob)
{
    return gob && gob_slot(gob) >= 0 && !gob_doomed(gob);
}

/* Name the peer knows the gob by: its own name if no other gob shares it, else (and for
 * unnamed actors) "n<host handle>"; the client creates its copy under that name.
 * Spawners name every spawn the same ("r3dogangen_spawn"). */
void gob_netname(BYTE *gob, char *out, int size)
{
    int slot;

    snprintf(out, size, "%s", gob_name(gob));
    if (!gob || !(gob_is_actor(gob) || gob_is_projectile(gob)))
        return;
    if (out[0] && name_unique(gob, out))
        return;
    out[0] = 0;
    if (!role_host) {
        out[0] = 0;
        return;
    }
    slot = gob_slot(gob);
    if (slot >= 0)
        snprintf(out, size, "n%x", gob_handle(slot));
}

/* A player's character by slot: our own player or that player's puppet */
static BYTE *player_gob(int slot)
{
    if (slot == my_slot)
        return local_player;
    return slot >= 0 && slot < MAX_PLAYERS ? puppets[slot] : NULL;
}

/* Slot of the player whose puppet this is, or -1 */
int puppet_slot(BYTE *gob)
{
    int s;

    for (s = 0; gob && s < MAX_PLAYERS; s++)
        if (puppets[s] == gob)
            return s;
    return -1;
}

/* The live puppet nearest pos, or NULL if there is none */
BYTE *puppet_nearest(const float *pos)
{
    BYTE *best = NULL;
    float bestd = 0.0f;
    int s;

    for (s = 0; s < MAX_PLAYERS; s++) {
        float *p, d;
        if (!gob_alive(puppets[s]))
            continue;
        p = (float *)(puppets[s] + GOB_POSITION);
        d = (p[0] - pos[0]) * (p[0] - pos[0]) + (p[1] - pos[1]) * (p[1] - pos[1]) + (p[2] - pos[2]) * (p[2] - pos[2]);
        if (!best || d < bestd) {
            best = puppets[s];
            bestd = d;
        }
    }
    return best;
}

/* How the other games should name one of our gobs: a player's character as "@p<number>"
 * (our player, or a puppet), the same in every game; anything else by network name */
void gob_peername(BYTE *gob, char *out, int size)
{
    int s = puppet_slot(gob);

    if (!gob)
        out[0] = 0;
    else if (gob == local_player && my_slot >= 0)
        snprintf(out, size, "@p%d", my_slot + 1);
    else if (s >= 0)
        snprintf(out, size, "@p%d", s + 1);
    else
        gob_netname(gob, out, size);
}

/* The reverse, on the receiving side: NULL if we have nothing by that name */
BYTE *gob_find_peername(const char *name)
{
    BYTE *part = NULL;
    int i;

    if (!name[0])
        return NULL;
    if (name[0] == '@' && name[1] == 'p')
        return player_gob(atoi(name + 2) - 1);
    if (name[0] == 'n' && name[1] && strspn(name + 1, "0123456789abcdef") == strlen(name + 1)) {
        unsigned int id = strtoul(name + 1, NULL, 16);
        return role_host ? gob_by_handle(id) : ent_gob(id);
    }
    /* a gob with object info first: parts without any can share their owner's name */
    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *g = (BYTE *)GOB_TABLE[i * 2];
        if (g && strncmp((const char *)g + GOB_NAME, name, 32) == 0 && !gob_doomed(g)) {
            if (*(BYTE **)(g + GOB_INFO))
                return g;
            if (!part)
                part = g;
        }
    }
    return part;
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

/* New level: every gob of the old one is gone, and a new gob at a freed address must not
 * inherit its flags (an enemy taking an old puppet's address would not think) */
void gob_sets_clear(void)
{
    memset(mirror_set, 0, sizeof(mirror_set));
    memset(foreign_set, 0, sizeof(foreign_set));
}

/* Delete every gob whose name starts with prefix and continues with digits ("coop_p" ->
 * coop_p1, coop_p2...: puppets an instant load brought back from the save); returns how many */
int gob_purge_named(const char *prefix)
{
    size_t n = strlen(prefix);
    int i, count = 0;

    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *g = (BYTE *)GOB_TABLE[i * 2];
        const char *name;
        if (!g)
            continue;
        name = (const char *)g + GOB_NAME;
        if (strncmp(name, prefix, n) == 0 && name[n] && strspn(name + n, "0123456789") == strlen(name + n)) {
            logmsg("purge %s (from the save)", name);
            gob_delete(g);
            count++;
        }
    }
    return count;
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

/* Class slots mirrored gobs skip: their logic runs in the peer's game, which sends the
 * results. think (+0x4): AI, shot flight. anim loop (+0x2c, called when an animation
 * loops, with the Lua OnAnimLoop): Max's deletes the gob an arm holds by handle, and the
 * puppet Max holds nothing, so copying the partner's animations crashed level 2. */
#define FILTER_SLOTS 2
static const int filter_slot[FILTER_SLOTS] = { CLASS_THINK, CLASS_ANIMLOOP };
static DWORD slot_orig[FILTER_SLOTS][64];

/* Called by a slot thunk: the original for normal gobs, 0 to skip mirrors */
static DWORD __cdecl slot_filter0(BYTE *gob)
{
    return gob_is_mirrored(gob) ? 0 : slot_orig[0][gob_class(gob)];
}

static DWORD __cdecl slot_filter1(BYTE *gob)
{
    return gob_is_mirrored(gob) ? 0 : slot_orig[1][gob_class(gob)];
}

static void *const slot_filters[FILTER_SLOTS] = { (void *)slot_filter0, (void *)slot_filter1 };

/* Thunk: push [esp+4]; call filter; add esp,4; test eax,eax; jz +2; jmp eax; xor eax,eax; ret
 * (the slot's own arguments stay on the stack for the jmp) */
static BYTE *make_thunk(void *filter)
{
    static const BYTE code[21] = {
        0xff, 0x74, 0x24, 0x04,         /* push dword [esp+4] */
        0xe8, 0, 0, 0, 0,               /* call filter */
        0x83, 0xc4, 0x04,               /* add esp, 4 */
        0x85, 0xc0,                     /* test eax, eax */
        0x74, 0x02,                     /* jz skip */
        0xff, 0xe0,                     /* jmp eax */
        0x31, 0xc0,                     /* skip: xor eax, eax */
        0xc3,                           /* ret */
    };
    BYTE *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    if (!t)
        return NULL;
    memcpy(t, code, sizeof(code));
    *(DWORD *)(t + 5) = (DWORD)filter - (DWORD)(t + 9);
    return t;
}

/* Route a class's filtered slots through the filters so mirrored gobs skip them */
void class_filter_think(int cls)
{
    static BYTE *thunks[FILTER_SLOTS];
    DWORD *desc, old;
    int f, k;

    if (cls <= 0 || cls >= 64)
        return;
    desc = (DWORD *)CLASS_TABLE[cls];
    for (f = 0; f < FILTER_SLOTS; f++) {
        int slot = filter_slot[f];
        DWORD orig;

        if (slot_orig[f][cls] || !desc[slot])
            continue;
        if (!thunks[f] && !(thunks[f] = make_thunk(slot_filters[f])))
            continue;
        orig = desc[slot];
        if (orig == (DWORD)thunks[f])
            continue;
        /* classes can share a descriptor: give them all the original */
        for (k = 1; k < 64; k++)
            if (CLASS_TABLE[k] == (BYTE *)desc)
                slot_orig[f][k] = orig;
        VirtualProtect(desc, CLASS_SLOTS * 4, PAGE_READWRITE, &old);
        desc[slot] = (DWORD)thunks[f];
        VirtualProtect(desc, CLASS_SLOTS * 4, old, &old);
        logmsg("class %d: slot +0x%x %lx filtered (mirrored gobs skip it)", cls, slot * 4, (unsigned long)orig);
    }
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
