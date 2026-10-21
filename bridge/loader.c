/*
 * The settings.lua loader line. The game runs %LOCALAPPDATA%\MDK2HD\settings.lua at boot,
 * after its own scripts; one line there calls coop_init (dinput8.c repoints a Lua builtin
 * to it). The game rewrites that file from scratch when options are saved
 * (mdkSaveControlSettings, the options menu), which drops the line, and the next start
 * would have no co-op. The bridge is loaded with the exe, before the game reads the file,
 * so it puts the line back then. Same block as tools/inject.sh and the installer write.
 */
#include <stdio.h>
#include <string.h>
#include "bridge.h"

#define LOADER_BEGIN    "-- >>> mdk2-coop >>>"
#define LOADER_BLOCK    "\r\n" LOADER_BEGIN "\r\nif (coop_init) then coop_init() end\r\n-- <<< mdk2-coop <<<\r\n"
/* the game reads settings.lua into an 8 KB stack buffer and crashes when it is larger */
#define SETTINGS_MAX    8000

void loader_ensure(void)
{
    char path[MAX_PATH], buf[SETTINGS_MAX + 1];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH);
    FILE *f;
    size_t len;

    if (!n || n >= MAX_PATH - 32)
        return;
    strcat(path, "\\MDK2HD");
    CreateDirectoryA(path, NULL);
    strcat(path, "\\settings.lua");
    if (!(f = fopen(path, "rb"))) {
        /* first start of the game: it boots with defaults from a settings.lua holding only this */
        if ((f = fopen(path, "wb"))) {
            fputs(LOADER_BLOCK + 2, f);
            fclose(f);
            logmsg("loader: no settings.lua yet: made one with the loader line");
        }
        return;
    }
    len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;
    if (strstr(buf, LOADER_BEGIN)) {
        logmsg("loader: line present in settings.lua");
        return;
    }
    if (len + strlen(LOADER_BLOCK) > SETTINGS_MAX) {
        logmsg("loader: settings.lua is %u bytes, no room for the loader line: co-op is off", (unsigned)len);
        return;
    }
    if (!(f = fopen(path, "ab"))) {
        logmsg("loader: cannot write %s: co-op is off", path);
        return;
    }
    fputs(LOADER_BLOCK, f);
    fclose(f);
    logmsg("loader: the game had rewritten settings.lua without the loader line: put back");
}
