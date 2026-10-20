/*
 * Per-frame tick. The main loop calls update_scene(scene) (0x457520) every frame,
 * menus and title screen included; redirecting that call lets the bridge run
 * coop.Tick() in Lua right after the scene has updated, outside any other Lua call.
 */
#include "bridge.h"

#define UPDATE_SCENE_ADDR   0x457520
#define UPDATE_SCENE_CALL   ((BYTE *)0x402638)      /* call update_scene(scene) */

static const char tick_lua[] = "if (coop and coop.Tick) then coop.Tick() end";

static void __cdecl update_scene_and_tick(void *scene)
{
    ((void (*)(void *))UPDATE_SCENE_ADDR)(scene);
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
