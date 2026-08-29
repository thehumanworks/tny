# tnytty — the tiny terminal (tny monorepo sibling app; root ADR 0045).
# Targets: make (release), make debug, make test, make size, make warn-strict

CC  ?= cc
GIT ?= git
STD  = -std=c11
WARN = -Wall -Wextra -Werror
ROOT = ..
TP_DIR = $(ROOT)/third_party
INC  = -Isrc -I$(TP_DIR)/yyjson -I$(TP_DIR)/picohttpparser -I$(TP_DIR)/greatest
# _XOPEN_SOURCE for posix_openpt/grantpt; _DEFAULT_SOURCE/_DARWIN_C_SOURCE
# re-enable the BSD extras (cfmakeraw, TIOCGWINSZ) alongside it.
DEFS = -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_DARWIN_C_SOURCE

TNYTTY_VERSION ?= $(shell $(GIT) describe --tags --always --dirty 2>/dev/null | sed 's/^v//')
ifeq ($(strip $(TNYTTY_VERSION)),)
  TNYTTY_VERSION := 0.0.0-unknown
endif
DEFS += -DTNYTTY_VERSION=\"$(TNYTTY_VERSION)\"

UNAME_S := $(shell uname -s 2>/dev/null || echo unknown)

REL_CFLAGS = $(STD) $(WARN) $(INC) $(DEFS) -Os -ffunction-sections -fdata-sections
DBG_CFLAGS = $(STD) $(WARN) $(INC) $(DEFS) -O0 -g

ifeq ($(UNAME_S),Darwin)
  REL_LDFLAGS = -Wl,-dead_strip
  DBG_LDFLAGS =
else
  REL_LDFLAGS = -Wl,--gc-sections
  DBG_LDFLAGS =
endif

SANITIZE ?= 1
ifeq ($(SANITIZE),1)
  DBG_CFLAGS  += -fsanitize=address,undefined
  DBG_LDFLAGS += -fsanitize=address,undefined
endif

BUILD   = build
OBJ_REL = $(BUILD)/rel
OBJ_DBG = $(BUILD)/dbg
BIN     = $(BUILD)/tnytty
TEST_BIN = $(BUILD)/tnytty-test

SRC = $(wildcard src/*.c src/vt/*.c src/term/*.c src/session/*.c src/api/*.c \
      src/icat/*.c src/util/*.c)
# Shared vendored libraries, pinned once at the monorepo root.
TP  = $(TP_DIR)/yyjson/yyjson.c $(TP_DIR)/picohttpparser/picohttpparser.c
TEST_SRC = $(wildcard tests/*.c)

REL_OBJS  = $(SRC:%.c=$(OBJ_REL)/%.o) $(TP:$(TP_DIR)/%.c=$(OBJ_REL)/tp/%.o)
DBG_DEPS  = $(filter-out src/main.c,$(SRC))
TEST_OBJS = $(DBG_DEPS:%.c=$(OBJ_DBG)/%.o) $(TP:$(TP_DIR)/%.c=$(OBJ_DBG)/tp/%.o) \
            $(TEST_SRC:%.c=$(OBJ_DBG)/%.o)

all: release
release: $(BIN)

$(BIN): $(REL_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(REL_CFLAGS) -o $@ $^ $(REL_LDFLAGS)

$(OBJ_REL)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(REL_CFLAGS) -c -o $@ $<

# third_party compiles without -Werror (vendored, exempt from our gates)
$(OBJ_REL)/tp/%.o: $(TP_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -Werror,$(REL_CFLAGS)) -c -o $@ $<

$(OBJ_DBG)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(DBG_CFLAGS) -c -o $@ $<

$(OBJ_DBG)/tp/%.o: $(TP_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -Werror,$(DBG_CFLAGS)) -c -o $@ $<

$(TEST_BIN): $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(DBG_CFLAGS) -o $@ $^ $(DBG_LDFLAGS)

debug: $(TEST_BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

size: $(BIN)
	@cp $(BIN) $(BUILD)/tnytty.stripped
	@strip $(BUILD)/tnytty.stripped 2>/dev/null || true
	@wc -c < $(BUILD)/tnytty.stripped | awk '{printf "tnytty stripped: %d bytes (%.1f KiB)\n", $$1, $$1/1024}'

# Stricter first-party diagnostics, mirroring the root quality gates
# (root ADR 0039). Root `make quality` already format-checks these files.
WARN_STRICT = -Wpedantic -Wformat=2 -Wno-format-nonliteral -Wshadow -Wstrict-prototypes \
              -Wmissing-prototypes -Wundef -Wwrite-strings -Wvla
warn-strict:
	$(CC) $(REL_CFLAGS) $(WARN_STRICT) -fsyntax-only $(SRC)

clean:
	rm -rf $(BUILD)

.PHONY: all release debug test size warn-strict clean
