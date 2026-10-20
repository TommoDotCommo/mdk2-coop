/*
 * Per-frame tick. The main loop calls update_scene(scene) (0x457520) every frame,
 * menus and title screen included; redirecting that call lets the bridge run
 * coop.Tick() in Lua right after the scene has updated, outside any other Lua call.
 */
#include <stdio.h>
#include <stdlib.h>
#include "bridge.h"

#define UPDATE_SCENE_ADDR   0x457520
#define UPDATE_SCENE_CALL   ((BYTE *)0x402638)      /* call update_scene(scene) */

static const char tick_lua[] = "if (coop and coop.Tick) then coop.Tick() end";

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
    ((void (*)(void *))UPDATE_SCENE_ADDR)(scene);
    run_command_file();
    lua_dobuffer(tick_lua, sizeof(tick_lua) - 1, "coop tick");
}

void frame_init(void)
{
    BYTE *call = UPDATE_SCENE_CALL;
    DWORD old;

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
