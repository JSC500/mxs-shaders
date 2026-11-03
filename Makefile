# Linux build of mxs-shaders: one LD_PRELOAD library, build/libmxs-shaders.so.
# Needs gcc, make, and the X11 and FreeType headers:
#   Debian/Ubuntu: sudo apt install build-essential libx11-dev libfreetype-dev
#   Fedora:        sudo dnf install gcc make libX11-devel freetype-devel
#   Arch:          sudo pacman -S base-devel libx11 freetype2
# FreeType is only needed at build time for its headers. The library loads libfreetype.so.6
# when the F8 panel first opens, so the game still starts without it.
# build.bat is untouched and still builds the Windows DLL.

CC      ?= gcc
CFLAGS  ?= -O2 -g
# What the build needs, kept apart from CFLAGS so "make CFLAGS=..." can't drop it.
FT_CFLAGS := $(shell pkg-config --cflags freetype2 2>/dev/null || echo -I/usr/include/freetype2)
MXS_CFLAGS = -std=gnu11 -fPIC -fvisibility=hidden -fno-strict-aliasing -Isrc $(FT_CFLAGS) \
             -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-missing-braces -Wno-unused-function \
             $(GCC14_ERRORS)
# GCC 14 and newer refuse these by default. Making them errors on older compilers too keeps the build
# from passing on one machine and failing on another.
GCC14_ERRORS = -Werror=incompatible-pointer-types -Werror=implicit-function-declaration \
               -Werror=int-conversion -Werror=implicit-int
MXS_LDFLAGS = -shared -Wl,-z,defs -Wl,--as-needed
LDLIBS  += -lX11 -ldl -lm

# The shared code, the Linux layer, and the default pack baked in like build.bat does.
SHARED = src/hooks.c src/post.c src/pack.c src/ui.c
LINUX  = src/preload.c src/platform_linux.c src/input_x11.c src/log_linux.c src/win_compat_linux.c
OBJ    = $(patsubst src/%.c,build/linux/%.o,$(SHARED) $(LINUX)) build/linux/default_pack.o
LIB    = build/libmxs-shaders.so
PACK   = $(wildcard shaderpacks/default/*)

all: $(LIB)

build/linux/%.o: src/%.c src/*.h | build/linux
	$(CC) $(MXS_CFLAGS) $(CFLAGS) -c -o $@ $<

build/linux/gen_pack: scripts/gen_pack.c | build/linux
	$(CC) -O2 -o $@ $<

build/linux/default_pack.c: build/linux/gen_pack $(PACK)
	build/linux/gen_pack shaderpacks/default $@

build/linux/default_pack.o: build/linux/default_pack.c src/common.h
	$(CC) $(MXS_CFLAGS) $(CFLAGS) -c -o $@ $<

$(LIB): $(OBJ)
	$(CC) $(MXS_CFLAGS) $(CFLAGS) $(MXS_LDFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/linux:
	mkdir -p $@

# The symbols the library exports. Everything else must stay hidden.
exports: $(LIB)
	@nm -D --defined-only $(LIB) | awk '$$2 == "T" {print $$3}' | sort

# Needs Xvfb, Mesa and libXtst. MXS_GAME_SHADERS=<folder> adds a check that the default pack
# compiles and links against the game's own shaders (from a probe run; they aren't in the repo).
test: $(LIB)
	tests/run.sh

clean:
	rm -rf build/linux $(LIB) build/tests

.PHONY: all exports test clean
