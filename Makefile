CC      = i686-w64-mingw32-gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11 -fno-strict-aliasing
LDFLAGS = -shared -static -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fixup
GAMEDIR ?= $(HOME)/.local/share/Steam/steamapps/common/Resident Evil 6
SRC     = src/proxy.c src/gyro.c src/hook.c src/config.c

build/xinput1_3.dll: $(SRC) src/common.h src/steaminput.h src/xinput1_3.def
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) src/xinput1_3.def $(LDFLAGS)

install: build/xinput1_3.dll
	rm -f "$(GAMEDIR)/xinput1_3.dll"
	cp build/xinput1_3.dll "$(GAMEDIR)/"

uninstall:
	rm -f "$(GAMEDIR)/xinput1_3.dll"

clean:
	rm -rf build

.PHONY: install uninstall clean
