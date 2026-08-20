# tny — C11 TUI + CLI coding-agent harness.
# Targets: make (release), make debug, make test, make size, make bench

CC      ?= cc
STD      = -std=c11
WARN     = -Wall -Wextra -Werror -Wno-deprecated-declarations
INC      = -Isrc -Ithird_party -Ithird_party/yyjson -Ithird_party/picohttpparser \
           -Ithird_party/wslay -Ithird_party/wslay/wslay -Ithird_party/greatest
DEFS     = -DHAVE_ARPA_INET_H -DHAVE_NETINET_IN_H -D_DARWIN_C_SOURCE

UNAME_S := $(shell uname -s)

REL_CFLAGS = $(STD) $(WARN) $(INC) $(DEFS) -Os -ffunction-sections -fdata-sections
DBG_CFLAGS = $(STD) $(WARN) $(INC) $(DEFS) -O0 -g -fsanitize=address,undefined

# SecureTransport is dlopen'd at first TLS use (src/net/stream.c) — do NOT
# link the frameworks; eager loading costs ~1.2 ms at every launch.
ifeq ($(UNAME_S),Darwin)
  # pthreads live in libSystem; no extra flags needed
  REL_LDFLAGS = -Wl,-dead_strip
  DBG_LDFLAGS = -fsanitize=address,undefined
else
  REL_CFLAGS += -pthread
  DBG_CFLAGS += -pthread
  REL_LDFLAGS = -Wl,--gc-sections -pthread
  DBG_LDFLAGS = -fsanitize=address,undefined -pthread
endif

BUILD    = build
OBJ_REL  = $(BUILD)/rel
OBJ_DBG  = $(BUILD)/dbg

SRC := $(wildcard src/*.c src/util/*.c src/json/*.c src/core/*.c src/cli/*.c \
        src/net/*.c src/mcp/*.c src/tui/*.c \
        src/backends/openai/*.c src/backends/acp/*.c src/backends/codex/*.c \
        src/backends/cursor/*.c)

TP  := third_party/yyjson/yyjson.c third_party/picohttpparser/picohttpparser.c \
       third_party/wslay/wslay_event.c third_party/wslay/wslay_frame.c \
       third_party/wslay/wslay_net.c third_party/wslay/wslay_queue.c \
       third_party/wslay/wslay_stack.c

REL_OBJS := $(SRC:%.c=$(OBJ_REL)/%.o) $(TP:%.c=$(OBJ_REL)/%.o)

TEST_SRC := $(wildcard tests/*.c)
TEST_DEPS := $(filter-out src/main.c,$(SRC)) $(TP)
TEST_OBJS := $(TEST_DEPS:%.c=$(OBJ_DBG)/%.o)

.PHONY: all release debug test size bench clean

all: release

release: $(BUILD)/tny

$(BUILD)/tny: $(REL_OBJS)
	@mkdir -p $(@D)
	$(CC) $(REL_CFLAGS) -o $@ $^ $(REL_LDFLAGS)
	strip $@ 2>/dev/null || strip -x $@
	@wc -c $@

$(OBJ_REL)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(REL_CFLAGS) -MMD -MP $(if $(findstring third_party,$<),-Wno-error -w,) -c -o $@ $<

$(OBJ_DBG)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(DBG_CFLAGS) -MMD -MP $(if $(findstring third_party,$<),-Wno-error -w,) -c -o $@ $<

$(BUILD)/tny-test: $(TEST_OBJS) $(TEST_SRC:%.c=$(OBJ_DBG)/%.o)
	@mkdir -p $(@D)
	$(CC) $(DBG_CFLAGS) -o $@ $^ $(DBG_LDFLAGS)

debug: $(BUILD)/tny-test

test: $(BUILD)/tny-test release
	./$(BUILD)/tny-test
	@if [ -x tests/integration/run.sh ]; then tests/integration/run.sh; fi

size: release
	@wc -c $(BUILD)/tny

bench: release
	hyperfine --warmup 5 -N './$(BUILD)/tny --version'

clean:
	rm -rf $(BUILD)

# Header dependencies emitted by -MMD; a header edit rebuilds its users.
-include $(REL_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(TEST_SRC:%.c=$(OBJ_DBG)/%.d)
