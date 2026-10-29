CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -shared -static-libgcc -Wl,--enable-stdcall-fixup -Wl,--kill-at -lws2_32 -ldxguid -lgdi32 -lwinmm -lshell32 -lwininet
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

GAME_DIR ?=

.DEFAULT_GOAL := dinput8.dll

SRCS = combat.c combat_core.c combat_hud.c dinput8.c game.c net.c steam.c entities.c shots.c pickups.c doors.c rooms.c frame.c input.c state.c saves.c settings.c loader.c launcher.c hud.c ctrlview.c netstats.c revive.c chat.c movie.c emote.c voice.c lua.c route.c bot.c pad.c diag.c redirect.c scripttrace.c remote.c presence.c presence_game.c

# the diagnostics bundle script, embedded as a C string so it always matches this DLL
diag_ps1.h: ../dist/diag.ps1
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\r$$//' -e 's/^/"/' -e 's/$$/\\n"/' $< | \
		{ echo 'static const char diag_script[] ='; cat; echo ';'; } > $@

dinput8.dll: $(SRCS) bridge.h anim_guard.h crash_context.h combat_core.h combat_hud.h presence.h snapshot.h steam_abi.h route.h pad.h launcher_session.h launcher_profiles.h launcher_profiles_win.h launcher_update.h launcher_update_win.h launcher_voice_win.h dinput8.def diag_ps1.h
	$(CC) $(CFLAGS) -DMOD_VERSION='"$(VERSION)"' -o $@ $(SRCS) dinput8.def $(LDFLAGS)

install: dinput8.dll
	@test -n "$(GAME_DIR)" || { echo "Set GAME_DIR to a disposable test copy" >&2; exit 1; }
	cp dinput8.dll "$(GAME_DIR)/dinput8.dll"

clean:
	rm -f dinput8.dll diag_ps1.h

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

test-combat:
	python3 ../tools/test_combat.py

.PHONY: install clean test-shots test-pad test-revive test-combat

../local/tests/redirect_test.exe: tests/redirect_test.c redirect.c bridge.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -o $@ tests/redirect_test.c -static-libgcc -lshell32

test-redirect: ../local/tests/redirect_test.exe
	../local/tests/redirect_test.exe

.PHONY: test-redirect

test-crash-context:
	../tools/test_crash_context.sh

.PHONY: test-crash-context
