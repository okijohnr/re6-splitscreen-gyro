CC      = i686-w64-mingw32-gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11 -fno-strict-aliasing
LDFLAGS = -shared -static -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fixup
# Game folder for `make install`. Put your own path in config.mk (not tracked),
# e.g.  GAMEDIR = /path/to/SteamLibrary/steamapps/common/Resident Evil 6
-include config.mk
GAMEDIR ?= $(HOME)/.local/share/Steam/steamapps/common/Resident Evil 6
SRC     = src/proxy.c src/gyro.c src/hook.c src/config.c src/debug.c

build/xinput1_3.dll: $(SRC) src/common.h src/steaminput.h src/xinput1_3.def
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) src/xinput1_3.def $(LDFLAGS)

build/re6gyro.ini: src/config.c tools/gen_ini.py
	@mkdir -p build
	python3 tools/gen_ini.py src/config.c $@

build/release.zip: build/xinput1_3.dll build/re6gyro.ini README.md LICENSE
	rm -f $@
	zip -j -X $@ build/xinput1_3.dll build/re6gyro.ini README.md LICENSE

release: build/release.zip

install: build/xinput1_3.dll
	rm -f "$(GAMEDIR)/xinput1_3.dll"
	cp build/xinput1_3.dll "$(GAMEDIR)/"

uninstall:
	rm -f "$(GAMEDIR)/xinput1_3.dll"

clean:
	rm -rf build

.PHONY: release install uninstall clean
