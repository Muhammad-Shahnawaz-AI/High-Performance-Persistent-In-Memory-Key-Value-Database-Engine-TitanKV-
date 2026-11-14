CC ?= gcc
CFLAGS = -std=c17 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Werror -pedantic -Iinclude
LDFLAGS = -lpthread -lm

RELEASE_FLAGS = -O2 -DNDEBUG
DEBUG_FLAGS = -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer

SRCS = src/allocator.c src/hashtable.c src/avl.c src/minheap.c src/protocol.c src/wal.c src/eventloop.c src/main.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean debug test

all: CFLAGS += $(RELEASE_FLAGS)
all: titankv-server bench

titankv-server: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

bench: tests/bench_and_verify.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

debug: CFLAGS += $(DEBUG_FLAGS)
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean titankv-server bench

clean:
	rm -f src/*.o titankv-server bench
	rm -rf ./data