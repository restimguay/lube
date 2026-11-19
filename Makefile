# Lube - build with `make`, test with `make test`.
# Needs only a C99 compiler and its standard library.

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra -Wno-unused-parameter -pedantic
LDLIBS  += -lm -lpthread
UNAME   := $(shell uname -s 2>/dev/null)
EXE     :=

# Give the interpreter a 64 MB stack on macOS and Windows so deep recursion
# works. (On Linux the main thread's stack is set by `ulimit -s`, usually
# 8 MB: raise it and pass STACK=... to use more.)
ifeq ($(UNAME),Darwin)
  STACK   ?= 0x3F00000
  LDFLAGS += -Wl,-stack_size,$(STACK)
else ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME)))
  STACK   ?= 0x3F00000
  LDFLAGS += -Wl,--stack,$(STACK)
  LDLIBS  := -lm  # threads come from Win32 itself
  EXE     := .exe
endif
ifdef STACK
  CFLAGS  += -DLUBE_STACK_SIZE=$(STACK)
endif

SRC  := $(wildcard src/*.c)
OBJ  := $(SRC:src/%.c=build/%.o)
PREFIX ?= /usr/local

lube$(EXE): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

ifneq ($(EXE),)
lube: lube$(EXE)
.PHONY: lube
endif

build/%.o: src/%.c src/lube.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

# A checking build: AddressSanitizer and UndefinedBehaviorSanitizer catch
# any out-of-bounds access, use-after-free or undefined behaviour, and the
# interpreter verifies at exit that every object was freed (no leaks).
lube-check: $(SRC) src/lube.h
	$(CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
		-fno-sanitize-recover=undefined -DLUBE_CHECK_LEAKS $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

# A ThreadSanitizer build: catches data races between isolates (threads).
lube-tsan: $(SRC) src/lube.h
	$(CC) $(CFLAGS) -O1 -g -fsanitize=thread $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

test: lube$(EXE)
	@sh tests/run.sh ./lube

check: lube-check
	@ASAN_OPTIONS=detect_leaks=0 sh tests/run.sh ./lube-check

tsan: lube-tsan
	@sh tests/run.sh ./lube-tsan

# Line, function, region and branch coverage of the test suite (needs clang's
# llvm-profdata and llvm-cov). Fails unless every line and function runs.
coverage:
	@sh tests/coverage.sh

install: lube$(EXE)
	install -d $(PREFIX)/bin
	install -m 755 lube$(EXE) $(PREFIX)/bin/lube$(EXE)

clean:
	rm -rf build lube lube.exe lube-check lube-check.dSYM lube-tsan lube-tsan.dSYM

.PHONY: test check tsan coverage install clean
