CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -shared -static-libgcc -Wl,--enable-stdcall-fixup -Wl,--kill-at -lws2_32 -ldxguid -lgdi32 -lwinmm

GAME_DIR ?= /mnt/c/coop/mdk2-test

SRCS = dinput8.c game.c net.c steam.c entities.c shots.c pickups.c doors.c rooms.c frame.c input.c state.c saves.c settings.c loader.c hud.c netstats.c revive.c chat.c lua.c route.c bot.c

dinput8.dll: $(SRCS) bridge.h steam_abi.h route.h dinput8.def
	$(CC) $(CFLAGS) -o $@ $(SRCS) dinput8.def $(LDFLAGS)

install: dinput8.dll
	cp dinput8.dll $(GAME_DIR)/dinput8.dll

clean:
	rm -f dinput8.dll

../local/tests/shots_test.exe: tests/shots_test.c shots.c bridge.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -o $@ tests/shots_test.c -static-libgcc

test-shots: ../local/tests/shots_test.exe
	../local/tests/shots_test.exe

.PHONY: install clean test-shots
