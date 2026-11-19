# Lube - build with `make`, test with `make test`.
# Needs only a C99 compiler and its standard library.

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra -Wno-unused-parameter -pedantic
LDLIBS  += -lm -lpthread

# Give the interpreter a 64 MB stack on macOS so deep recursion works.
# (On Linux the main thread stack is set by `ulimit -s`, usually 8 MB.)
ifeq ($(shell uname -s),Darwin)
  STACK   := 0x3F00000
  LDFLAGS += -Wl,-stack_size,$(STACK)
  CFLAGS  += -DLUBE_STACK_SIZE=$(STACK)
endif

SRC  := $(wildcard src/*.c)
OBJ  := $(SRC:src/%.c=build/%.o)
PREFIX ?= /usr/local

lube: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

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

test: lube
	@sh tests/run.sh ./lube

check: lube-check
	@ASAN_OPTIONS=detect_leaks=0 sh tests/run.sh ./lube-check

tsan: lube-tsan
	@sh tests/run.sh ./lube-tsan

# Line, function, region and branch coverage of the test suite (needs clang's
# llvm-profdata and llvm-cov). Fails unless every line and function runs.
coverage:
	@sh tests/coverage.sh

install: lube
	install -d $(PREFIX)/bin
	install -m 755 lube $(PREFIX)/bin/lube

clean:
	rm -rf build lube lube-check lube-check.dSYM lube-tsan lube-tsan.dSYM

.PHONY: test check tsan coverage install clean
