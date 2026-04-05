## Makefile for BTorrent — Educational BitTorrent Client
##
## Targets:
##   make          → build the btorrent binary
##   make test     → build & run unit tests
##   make clean    → remove all build artifacts
##   make debug    → build with debug symbols and AddressSanitizer

CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -Iinclude
LDFLAGS = -lcurl                       # libcurl for HTTP tracker requests

## Source files (add new .c files here)
SRCS = src/main.c     \
       src/bencode.c  \
       src/torrent.c  \
       src/sha1.c     \
       src/tracker.c  \
       src/peer.c     \
       src/pieces.c   \
       src/utils.c

OBJS  = $(patsubst src/%.c, build/%.o, $(SRCS))
BIN   = btorrent

## Default target: build the client binary
all: build_dir $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

## Compile each .c file into a .o in build/
build/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

## Create build directory if it doesn't exist
build_dir:
	@mkdir -p build

## Debug build: adds -g (symbols) and AddressSanitizer
debug: CFLAGS += -g -fsanitize=address,undefined -DDEBUG
debug: LDFLAGS += -fsanitize=address,undefined
debug: all

## Build and run bencoding unit tests
test: build_dir build/test_bencode
	@echo "=== Running bencoding tests ==="
	@./build/test_bencode

build/test_bencode: tests/test_bencode.c src/bencode.c src/utils.c src/sha1.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf build $(BIN)

.PHONY: all debug test clean build_dir
