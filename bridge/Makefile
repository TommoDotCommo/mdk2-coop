CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -shared -static-libgcc -Wl,--enable-stdcall-fixup -Wl,--kill-at -lws2_32 -ldxguid -lgdi32 -lwinmm -lshell32
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

GAME_DIR ?=

SRCS = dinput8.c game.c net.c steam.c entities.c shots.c pickups.c doors.c rooms.c frame.c input.c state.c saves.c settings.c loader.c launcher.c hud.c ctrlview.c netstats.c revive.c chat.c movie.c emote.c voice.c lua.c route.c bot.c pad.c

dinput8.dll: $(SRCS) bridge.h steam_abi.h route.h pad.h dinput8.def
	$(CC) $(CFLAGS) -DMOD_VERSION='"$(VERSION)"' -o $@ $(SRCS) dinput8.def $(LDFLAGS)

install: dinput8.dll
	@test -n "$(GAME_DIR)" || { echo "Set GAME_DIR to a disposable test copy" >&2; exit 1; }
	cp dinput8.dll "$(GAME_DIR)/dinput8.dll"

clean:
	rm -f dinput8.dll

../local/tests/shots_test.exe: tests/shots_test.c shots.c bridge.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -o $@ tests/shots_test.c -static-libgcc

test-shots: ../local/tests/shots_test.exe
	../local/tests/shots_test.exe

../local/tests/pad_test.exe: tests/pad_test.c pad.c pad.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -o $@ tests/pad_test.c -static-libgcc

test-pad: ../local/tests/pad_test.exe
	../local/tests/pad_test.exe

test-revive:
	../tools/test_revive.sh

.PHONY: install clean test-shots test-pad test-revive
