CC      = gcc
CSTD    = -std=c11
WARN    = -Wall -Wextra -Wpedantic -Wshadow
IFLAGS  = -Iinclude
PREFIX ?= /usr/local

SRCS = src/main.c          \
       src/cmd/cmd_download.c \
       src/cmd/cmd_inspect.c  \
       src/cmd/cmd_check.c    \
       src/scheduler.c        \
       src/net/tcp.c          \
       src/core/bencode.c  \
       src/core/torrent.c  \
       src/core/sha1.c     \
       src/core/pieces.c   \
       src/proto/peer.c    \
       src/proto/tracker.c \
       src/utils.c         \
       src/log.c           \
       src/result.c

OBJS = $(patsubst src/%.c, build/obj/%.o, $(SRCS))
BIN  = build/btorrent

## Release
all: CFLAGS  = $(CSTD) $(WARN) $(IFLAGS) -O2 -DNDEBUG -DLOG_MIN_LEVEL=1 \
               -D_FILE_OFFSET_BITS=64
all: LDFLAGS = -lcurl
all: $(BIN)

## Debug
debug: CFLAGS  = $(CSTD) $(WARN) $(IFLAGS) -g3 -O0 \
                 -fsanitize=address,undefined -DLOG_MIN_LEVEL=0 \
                 -D_FILE_OFFSET_BITS=64
debug: LDFLAGS = -lcurl -fsanitize=address,undefined
debug: $(BIN)

$(BIN): $(OBJS) | build/obj
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/obj/%.o: src/%.c | build/obj
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

build/obj:
	@mkdir -p build/obj/core build/obj/proto build/obj/net build/obj/cmd build/obj/scheduler

## Install
install: all
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/btorrent
	@echo "Installed to $(DESTDIR)$(PREFIX)/bin/btorrent"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/btorrent

## Tests
TEST_COMMON = src/utils.c src/log.c src/result.c
TEST_FLAGS  = $(CSTD) $(WARN) $(IFLAGS) -g3 -O0

test_sha1: build/obj
	$(CC) $(TEST_FLAGS) tests/unit/test_sha1.c src/core/sha1.c \
	    $(TEST_COMMON) -o build/test_sha1
	@echo "--- test_sha1 ---" && ./build/test_sha1

test_peer: build/obj
	$(CC) $(TEST_FLAGS) tests/unit/test_peer.c src/proto/peer.c \
	    src/core/sha1.c $(TEST_COMMON) -o build/test_peer
	@echo "--- test_peer ---" && ./build/test_peer

test_pieces: build/obj
	$(CC) $(TEST_FLAGS) tests/unit/test_pieces.c src/core/pieces.c \
	    src/core/sha1.c src/core/torrent.c src/core/bencode.c \
	    src/proto/peer.c $(TEST_COMMON) -o build/test_pieces
	@echo "--- test_pieces ---" && ./build/test_pieces

test: test_sha1 test_peer test_pieces

clean:
	rm -rf build

.PHONY: all debug test test_sha1 test_peer test_pieces install uninstall clean
