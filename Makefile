# Lube - build with `make`, test with `make test`.
# Needs only a C99 compiler and its standard library.
#
# `make` builds lube and, in plugins/ next to it, the plugins that come with
# it (fs, net, audio, gfx, array...: src/plugins/), which programs load when they import
# them. `make STATIC_PLUGINS=1` puts the plugins inside lube instead (one
# file: the static Linux release is built this way).

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra -Wno-unused-parameter -pedantic
LDLIBS  += -lm -lpthread
UNAME   := $(shell uname -s 2>/dev/null)
EXE     :=
PLUGINS := fs net audio gfx array math cmath random statistics nn learn llm web

# Give the interpreter a 64 MB stack on macOS and Windows so deep recursion
# works. (On Linux the main thread's stack is set by `ulimit -s`, usually
# 8 MB: raise it and pass STACK=... to use more.)
ifeq ($(UNAME),Darwin)
  STACK   ?= 0x3F00000
  LDFLAGS += -Wl,-stack_size,$(STACK)
  PLUGIN_EXT   := .dylib
  PLUGIN_FLAGS := -dynamiclib
else ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME)))
  STACK   ?= 0x3F00000
  LDFLAGS += -Wl,--stack,$(STACK)
  LDLIBS  := -lm  # threads come from Win32 itself
  EXE     := .exe
  PLUGIN_EXT   := .dll
  PLUGIN_FLAGS := -shared -static-libgcc  # (a DLL that needs no other of gcc's)
  PLUGIN_LIBS_net := -lws2_32  # sockets: Winsock
  PLUGIN_LIBS_web := -lws2_32
  PLUGIN_LIBS_gfx := -lgdi32 -luser32  # windows
else
  PLUGIN_EXT   := .so
  PLUGIN_FLAGS := -shared -fPIC
  ifeq ($(UNAME),Linux)
    LDLIBS += -ldl  # plugins (dlopen is in libdl before glibc 2.34)
    PLUGIN_LIBS_audio := -ldl  # sound: ALSA, opened when the first sound plays
    PLUGIN_LIBS_gfx := -ldl  # windows: X11, opened when the first window opens
    PLUGIN_LIBS_array := -ldl  # the system's BLAS (OpenBLAS), if it has one
  endif
endif
ifdef STACK
  CFLAGS  += -DLUBE_STACK_SIZE=$(STACK)
endif

SRC  := $(wildcard src/*.c)
PLUGIN_SRC := $(PLUGINS:%=src/plugins/%.c)
ifdef STATIC_PLUGINS
  SRC    += $(PLUGIN_SRC)
  CFLAGS += -DLUBE_STATIC_PLUGINS
  ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME)))
    LDLIBS += -lws2_32 -lgdi32 -luser32
  endif
  PLUGIN_FILES :=
else
  PLUGIN_FILES := $(PLUGINS:%=plugins/%$(PLUGIN_EXT))
endif
OBJ  := $(SRC:src/%.c=build/%.o)
LIB_OBJ := $(filter-out build/main.o,$(OBJ))  # (Lube for a host program: lube_embed.h)
EXE_OBJ := $(filter-out build/embed.o,$(OBJ))  # (and the lube command, which hosts no one)
PREFIX ?= /usr/local

# The checking builds have the plugins inside, so the sanitizers watch them too.
CHECK_SRC := $(wildcard src/*.c) $(PLUGIN_SRC)
PLUGIN_PARTS := $(wildcard src/plugins/*/*.c src/plugins/*.h) src/lube_gfx.h  # (what plugins include)
CHECK_LIBS := $(LDLIBS) $(PLUGIN_LIBS_net) $(PLUGIN_LIBS_gfx)

all: lube$(EXE) $(PLUGIN_FILES) liblube.a

lube$(EXE): $(EXE_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(EXE_OBJ) $(LDLIBS)

ifneq ($(EXE),)
lube: lube$(EXE)
.PHONY: lube
endif

# Lube inside another program: link liblube.a, include src/lube_embed.h (or .hpp).
lib: liblube.a

liblube.a: $(LIB_OBJ)
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJ)

build/%.o: src/%.c src/lube.h src/lube_plugin.h src/lube_embed.h src/interp_ops.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build build/plugins

# A plugin: a shared library that needs nothing of lube's (lube_plugin.h).
plugins/%$(PLUGIN_EXT): src/plugins/%.c src/lube_plugin.h | plugins
	$(CC) $(CFLAGS) $(PLUGIN_FLAGS) -o $@ $< -lm $(PLUGIN_LIBS_$*)

plugins/gfx$(PLUGIN_EXT): $(wildcard src/plugins/gfx/*.c) src/lube_gfx.h  # (its parts)
plugins/array$(PLUGIN_EXT): $(wildcard src/plugins/array/*.c) src/plugins/array.h src/plugins/vecmath.h
plugins/nn$(PLUGIN_EXT): $(wildcard src/plugins/nn/*.c) src/plugins/array.h src/plugins/vecmath.h
plugins/learn$(PLUGIN_EXT): $(wildcard src/plugins/learn/*.c) src/plugins/array.h
plugins/llm$(PLUGIN_EXT): $(wildcard src/plugins/llm/*.c) src/plugins/array.h
plugins/web$(PLUGIN_EXT): $(wildcard src/plugins/web/*.c)

plugins:
	mkdir -p plugins

# A checking build: AddressSanitizer and UndefinedBehaviorSanitizer catch
# any out-of-bounds access, use-after-free or undefined behaviour, and the
# interpreter verifies at exit that every object was freed (no leaks).
lube-check: $(CHECK_SRC) $(PLUGIN_PARTS) src/lube.h src/lube_plugin.h src/interp_ops.h
	$(CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -DLUBE_STATIC_PLUGINS \
		-fno-sanitize-recover=undefined -DLUBE_CHECK_LEAKS $(LDFLAGS) -o $@ $(CHECK_SRC) $(CHECK_LIBS)

# A ThreadSanitizer build: catches data races between isolates (threads).
lube-tsan: $(CHECK_SRC) $(PLUGIN_PARTS) src/lube.h src/lube_plugin.h src/interp_ops.h
	$(CC) $(CFLAGS) -O1 -g -fsanitize=thread -DLUBE_STATIC_PLUGINS $(LDFLAGS) -o $@ $(CHECK_SRC) $(CHECK_LIBS)

test: all
	@sh tests/run.sh ./lube

# (Lube checks its own leaks. It measures recursion by the addresses of
# locals, which ASan's use-after-return checking moves off the stack.)
# (The embedding tests build Lube into their host programs: with the same sanitizers here.)
check: lube-check
	@ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 \
		LUBE_EMBED_CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined" \
		sh tests/run.sh ./lube-check

tsan: lube-tsan
	@LUBE_EMBED_CFLAGS="-O1 -g -fsanitize=thread" sh tests/run.sh ./lube-tsan

# Line, function, region and branch coverage of the test suite (needs clang's
# llvm-profdata and llvm-cov). Fails unless every line and function runs.
coverage:
	@sh tests/coverage.sh

install: all
	install -d $(PREFIX)/bin
	install -m 755 lube$(EXE) $(PREFIX)/bin/lube$(EXE)
ifneq ($(PLUGIN_FILES),)
	install -d $(PREFIX)/lib/lube/plugins
	install -m 755 $(PLUGIN_FILES) $(PREFIX)/lib/lube/plugins/
endif

# lube built twice (needs clang and its llvm-profdata): first to count, as it runs bench/vs_python/core.lube,
# how often each branch of the interpreter goes each way, then with those counts to lay out its code by:
# the same results, about a tenth fewer instructions in the interpreter's loop, and calls a third faster.
PROFDATA ?= $(shell command -v llvm-profdata 2>/dev/null || xcrun -f llvm-profdata 2>/dev/null)
pgo: all
	@test -n "$(PROFDATA)" || { echo "make pgo needs clang's llvm-profdata"; exit 1; }
	rm -rf build/pgo build/*.o lube$(EXE)
	mkdir -p build/pgo
	CFLAGS="-O2 -fprofile-instr-generate" LDFLAGS="-fprofile-instr-generate" $(MAKE) lube$(EXE)
	LLVM_PROFILE_FILE=build/pgo/lube.profraw ./lube$(EXE) bench/vs_python/core.lube build/pgo 0.01 1 >/dev/null
	$(PROFDATA) merge -o build/pgo/lube.profdata build/pgo/lube.profraw
	rm -f build/*.o lube$(EXE)
	CFLAGS="-O2 -fprofile-instr-use=$(CURDIR)/build/pgo/lube.profdata -Wno-profile-instr-unprofiled" $(MAKE) all

clean:
	rm -rf build plugins lube lube.exe liblube.a lube-check lube-check.dSYM lube-tsan lube-tsan.dSYM

.PHONY: all lib test check tsan coverage install clean pgo
