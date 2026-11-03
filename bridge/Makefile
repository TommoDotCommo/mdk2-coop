CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter $(COOP_MUTANT_CFLAGS)
LDFLAGS = -Wl,--no-insert-timestamp -shared -static-libgcc -Wl,--enable-stdcall-fixup -Wl,--kill-at -lws2_32 -ldxguid -lgdi32 -lwinmm -lshell32 -lwininet
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

GAME_DIR ?=

.DEFAULT_GOAL := dinput8.dll

SRCS = combat.c combat_core.c combat_hud.c dinput8.c game.c net.c steam.c entities.c shots.c pickups.c doors.c rooms.c frame.c input.c state.c saves.c settings.c loader.c launcher.c hud.c ctrlview.c netstats.c revive.c chat.c movie.c emote.c voice.c plist.c lua.c route.c bot.c pad.c diag.c redirect.c scripttrace.c remote.c presence.c presence_game.c launcher_host.c

# VERSIONINFO with the mod version (tools/versioninfo.py rewrites version.rc only when the version changed)
VERSION_RC := $(shell python3 ../tools/versioninfo.py "$(VERSION)" dinput8.dll "MDK2 Co-op (DirectInput proxy)" version.rc >/dev/null && echo version.rc)

version_res.o: version.rc
	i686-w64-mingw32-windres version.rc -o $@

# the diagnostics bundle script, embedded as a C string so it always matches this DLL
diag_ps1.h: ../dist/diag.ps1
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\r$$//' -e 's/^/"/' -e 's/$$/\\n"/' $< | \
		{ echo 'static const char diag_script[] ='; cat; echo ';'; } > $@

dinput8.dll: $(SRCS) bridge.h anim_guard.h crash_context.h combat_core.h combat_hud.h presence.h snapshot.h steam_abi.h route.h pad.h launcher_session.h launcher_invite_command.h launcher_profiles.h launcher_join.h launcher_join.c launcher_host.h launcher_host_core.h launcher_host_win.h coop_invite.h coop_discover.h coop_discover_win.h launcher_update.h launcher_update_win.h launcher_voice_win.h launcher_look.h guide_keys.h dinput8.def diag_ps1.h version_res.o
	$(CC) $(CFLAGS) -DMOD_VERSION='"$(VERSION)"' -o $@ $(SRCS) version_res.o dinput8.def $(LDFLAGS)

install: dinput8.dll
	@test -n "$(GAME_DIR)" || { echo "Set GAME_DIR to a disposable test copy" >&2; exit 1; }
	cp dinput8.dll "$(GAME_DIR)/dinput8.dll"

clean:
	rm -f dinput8.dll diag_ps1.h version.rc version_res.o

../local/tests/shots_test.exe: tests/shots_test.c shots.c bridge.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -o $@ tests/shots_test.c -static-libgcc

test-shots: ../local/tests/shots_test.exe
	../local/tests/shots_test.exe

../local/tests/pad_test.exe: tests/pad_test.c pad.c pad.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -o $@ tests/pad_test.c -static-libgcc

../local/tests/pad_test_mutant.exe: tests/pad_test.c pad.c pad.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -DCOOP_MUTANT_PAD_NO_SWALLOW -o $@ tests/pad_test.c -static-libgcc

# the mutant (the D-pad not swallowed while View is held) must fail the same test
test-pad: ../local/tests/pad_test.exe ../local/tests/pad_test_mutant.exe
	../local/tests/pad_test.exe
	@if ../local/tests/pad_test_mutant.exe >/dev/null; then echo "MUTANT pad-no-swallow passed: the test cannot see it" >&2; exit 1; \
	else echo "mutant pad-no-swallow: fails as it must"; fi

../local/tests/launcher_look_test.exe: tests/launcher_look_test.c launcher_look.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -o $@ tests/launcher_look_test.c -static-libgcc

../local/tests/launcher_look_test_mutant.exe: tests/launcher_look_test.c launcher_look.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -DCOOP_MUTANT_LOOK_NO_FIT -o $@ tests/launcher_look_test.c -static-libgcc

# the launcher's game look: labels fit their buttons; the mutant (labels never shrink) must fail
test-launcher-look: ../local/tests/launcher_look_test.exe ../local/tests/launcher_look_test_mutant.exe
	../local/tests/launcher_look_test.exe
	@if ../local/tests/launcher_look_test_mutant.exe >/dev/null; then echo "MUTANT look-no-fit passed: the test cannot see it" >&2; exit 1; \
	else echo "mutant look-no-fit: fails as it must"; fi

test-revive:
	../tools/test_revive.sh

test-combat:
	python3 ../tools/test_combat.py

.PHONY: install clean test-shots test-pad test-launcher-look test-revive test-combat

../local/tests/redirect_test.exe: tests/redirect_test.c redirect.c bridge.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -o $@ tests/redirect_test.c -static-libgcc -lshell32

test-redirect: ../local/tests/redirect_test.exe
	../local/tests/redirect_test.exe

.PHONY: test-redirect

test-crash-context:
	../tools/test_crash_context.sh

.PHONY: test-crash-context

# invites and finding friends (coop_invite.h, coop_discover.h, coop_discover_win.h)
../local/tests/coop_invite_test.exe: tests/coop_invite_test.c coop_invite.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -std=c99 -I. -o $@ tests/coop_invite_test.c -static-libgcc

../local/tests/coop_discover_test.exe: tests/coop_discover_test.c coop_discover.h coop_invite.h launcher_profiles.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -std=c99 -I. -o $@ tests/coop_discover_test.c -static-libgcc

../local/tests/coop_discover_win_test.exe: tests/coop_discover_win_test.c coop_discover_win.h coop_discover.h coop_invite.h
	mkdir -p ../local/tests
	$(CC) $(CFLAGS) -std=c99 -I. -o $@ tests/coop_discover_win_test.c -static-libgcc -lws2_32

test-invite: ../local/tests/coop_invite_test.exe
	../local/tests/coop_invite_test.exe

test-discover: ../local/tests/coop_discover_test.exe
	../local/tests/coop_discover_test.exe

# loopback only; add --adapters by hand at the desk (Windows Firewall may ask about the test program)
test-discover-win: ../local/tests/coop_discover_win_test.exe
	../local/tests/coop_discover_win_test.exe

# the whole gate: sanitisers, fuzzing, mutants, C++ build, Windows adapter self-test
test-discovery:
	python3 ../tools/test_coop_discovery.py

.PHONY: test-invite test-discover test-discover-win test-discovery
