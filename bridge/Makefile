CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -shared -static-libgcc -Wl,--enable-stdcall-fixup -Wl,--kill-at -lws2_32

GAME_DIR ?= /mnt/c/coop/mdk2-test

SRCS = dinput8.c game.c net.c entities.c lua.c

dinput8.dll: $(SRCS) bridge.h dinput8.def
	$(CC) $(CFLAGS) -o $@ $(SRCS) dinput8.def $(LDFLAGS)

install: dinput8.dll
	cp dinput8.dll $(GAME_DIR)/dinput8.dll

clean:
	rm -f dinput8.dll

.PHONY: install clean
