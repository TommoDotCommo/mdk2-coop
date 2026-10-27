/*
 * Per-frame tick. The main loop calls update_scene(scene) (0x457520) every frame,
 * menus and title screen included; redirecting that call lets the bridge run
 * coop.Tick() in Lua right after the scene has updated, outside any other Lua call.
 */
#include <stdio.h>
#include <stdlib.h>
#include "bridge.h"

#define UPDATE_SCENE_ADDR   ADDR.update_scene
#define UPDATE_SCENE_CALL   ((BYTE *)0x402638)      /* call update_scene(scene) */

static const char tick_lua[] = "if (coop and coop.Tick) then coop.Tick() end";

/* A gob's +0x70 part of kind 1 (updated at 0x447ab0, a particle emitter) keeps an index into its particle
 * ring at +0x34. On a client it can be negative (-15 on level 2's movie camera, ml2b_cameragob, from the level
 * load on); when the torpedo minigame lets that camera go, the update moves index * 32 bytes of the ring and a
 * negative index copies gigabytes: the client crashed at 0x48b00d every time. Reset such a count before the
 * scene updates. coop.ini [test] emitter_guard=0 (from the start) or coop_emitter_guard(0) turns this off, for
 * the test that proves it is needed. */
#define PART_OF_GOB         0x70
#define EMITTER_KIND        1
#define EMITTER_RING_INDEX        0x34
int emitter_guard = 1;

static void fix_emitters(void)
{
    static BYTE *logged[16];
    static int nlogged;
    int i, j;

    if (!emitter_guard)
        return;
    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *gob = (BYTE *)GOB_TABLE[i * 2], *part;
        if (!gob || !(part = *(BYTE **)(gob + PART_OF_GOB)) || *(DWORD *)part != EMITTER_KIND ||
            *(int *)(part + EMITTER_RING_INDEX) >= 0)
            continue;
        /* the game sets it again every frame while the emitter is held: say so once per emitter */
        for (j = 0; j < nlogged && logged[j] != part; j++)
            ;
        if (j == nlogged && nlogged < 16) {
            logged[nlogged++] = part;
            logmsg("emitter %s: ring index %d, reset to 0", gob_name(gob), *(int *)(part + EMITTER_RING_INDEX));
        }
        *(int *)(part + EMITTER_RING_INDEX) = 0;
    }
}

/* Automation: every few frames, run and delete <game dir>\coop\command.lua if it exists
 * (tools/sweep.sh drives checkpoint loads this way instead of key presses, which a
 * Steam overlay or a focus change can swallow) */
static void run_command_file(void)
{
    static int frames;
    char path[MAX_PATH], *buf;
    FILE *f;
    long len;

    if (++frames % 10)
        return;
    snprintf(path, MAX_PATH, "%s\\command.lua", coop_dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES || !(f = fopen(path, "rb")))
        return;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(len + 1);
    if (buf && fread(buf, 1, len, f) == (size_t)len) {
        buf[len] = 0;
        fclose(f);
        DeleteFileA(path);
        logmsg("command.lua (%ld bytes): %s", len, buf);
        lua_dobuffer(buf, (int)len, "command.lua");
    } else {
        fclose(f);
    }
    free(buf);
}

static void __cdecl update_scene_and_tick(void *scene)
{
    input_pad_frame();
    bot_before_frame();
    fix_emitters();
    ((void (*)(void *))UPDATE_SCENE_ADDR)(scene);
    run_command_file();
    diag_tick();
    pickups_frame();
    lua_dobuffer(tick_lua, sizeof(tick_lua) - 1, "coop tick");
}

void frame_init(void)
{
    BYTE *call = UPDATE_SCENE_CALL;
    DWORD old;
    char ini[MAX_PATH];

    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    emitter_guard = GetPrivateProfileIntA("test", "emitter_guard", 1, ini) != 0;
    if (!emitter_guard)
        logmsg("frame: emitter guard off ([test] emitter_guard=0)");
    if (call[0] != 0xe8 || (DWORD)(call + 5) + *(DWORD *)(call + 1) != UPDATE_SCENE_ADDR) {
        logmsg("frame: unexpected code at %p, no tick", (void *)call);
        return;
    }
    VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(call + 1) = (DWORD)update_scene_and_tick - (DWORD)(call + 5);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    logmsg("frame: coop.Tick() runs after every scene update");
}
