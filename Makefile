# tny — C11 TUI + CLI coding-agent harness.
# Targets: make (release), make debug, make test, make size, make size-check,
#          make pack, make bench, make install, make site

CC      ?= cc
STD      = -std=c11
WARN     = -Wall -Wextra -Werror -Wno-deprecated-declarations
INC      = -Isrc -Ithird_party -Ithird_party/yyjson -Ithird_party/picohttpparser \
           -Ithird_party/wslay -Ithird_party/wslay/wslay -Ithird_party/greatest
DEFS     = -DHAVE_ARPA_INET_H -DHAVE_NETINET_IN_H -D_DARWIN_C_SOURCE \
           -D_DEFAULT_SOURCE -D_BSD_SOURCE

UNAME_S := $(shell uname -s 2>/dev/null || echo unknown)
UNAME_M := $(shell uname -m 2>/dev/null || echo unknown)

# Version comes from git at build time (docs/adr/0014): the nearest v* tag,
# plus -N-g<hash>[-dirty] between releases. Release CI overrides it with the
# pushed tag (TNY_VERSION env/arg) so shallow and containerized builds do not
# depend on tag fetching. Tarball builds without git fall back below.
TNY_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null | sed 's/^v//')
ifeq ($(strip $(TNY_VERSION)),)
  TNY_VERSION := 0.0.0-unknown
endif

# MSYS2/Cygwin are POSIX enough to compile the existing sources. Native
# Win32 (MSVC / MinGW without the MSYS runtime) is still later.
WINDOWS := 0
ifneq ($(filter MSYS% MINGW% CYGWIN%,$(UNAME_S)),)
  WINDOWS := 1
endif

ifeq ($(WINDOWS),1)
  EXE := .exe
else
  EXE :=
endif

BIN      = $(BUILD)/tny$(EXE)
TEST_BIN = $(BUILD)/tny-test$(EXE)

# STATIC=1: musl static publish build. Ignored on Darwin (cannot static-link).
STATIC   ?= 0
# SANITIZE=1: ASan/UBSan on the debug/test binary. Off for musl and MSYS2.
ifeq ($(WINDOWS),1)
  SANITIZE ?= 0
else
  SANITIZE ?= 1
endif

REL_CFLAGS = $(STD) $(WARN) $(INC) $(DEFS) -Os -ffunction-sections -fdata-sections
DBG_CFLAGS = $(STD) $(WARN) $(INC) $(DEFS) -O0 -g

# TLS is dlopen'd at first use (src/net/stream.c): SecureTransport on macOS,
# system libssl on Linux. Do NOT link frameworks or libssl; eager loading
# costs launch time and libssl would pin a link-time dependency.
ifeq ($(UNAME_S),Darwin)
  REL_LDFLAGS = -Wl,-dead_strip
  DBG_LDFLAGS =
else
  REL_CFLAGS += -pthread
  DBG_CFLAGS += -pthread
  REL_LDFLAGS = -Wl,--gc-sections -pthread
  DBG_LDFLAGS = -pthread
endif

# dlopen lives in libdl on glibc < 2.34; a no-op stub on newer glibc and musl.
ifeq ($(UNAME_S),Linux)
  REL_LDFLAGS += -ldl
  DBG_LDFLAGS += -ldl
endif

ifeq ($(STATIC),1)
  ifneq ($(UNAME_S),Darwin)
    REL_LDFLAGS += -static
  endif
endif

ifeq ($(SANITIZE),1)
  DBG_CFLAGS  += -fsanitize=address,undefined
  DBG_LDFLAGS += -fsanitize=address,undefined
endif

ifeq ($(WINDOWS),1)
  REL_LDFLAGS += -static-libgcc
endif

BUILD    = build
OBJ_REL  = $(BUILD)/rel
OBJ_DBG  = $(BUILD)/dbg
GEN      = $(BUILD)/generated
VERSION_H = $(GEN)/tny_version.h
INC     += -I$(GEN)

SRC_ALL := $(wildcard src/*.c src/util/*.c src/json/*.c src/core/*.c src/cli/*.c \
        src/net/*.c src/mcp/*.c src/tui/*.c \
        src/backends/openai/*.c src/backends/acp/*.c src/backends/codex/*.c \
        src/backends/cursor/*.c)

# Per-platform source lists (docs/adr/0017). Native transports (sockets, TLS,
# hand-rolled HTTP/1.1 + wslay WebSocket) and the poll(2) wrapper are excluded
# from the wasm build wholesale rather than #ifdef-riddled; src/net/net_wasm.c
# replaces the whole seam there (fetch, browser WebSocket, pseudo-fd registry).
SRC_NATIVE := src/net/tcp.c src/net/stream.c src/net/http1.c src/net/ws.c \
              src/util/tny_poll.c
SRC_WASM_ONLY := src/net/net_wasm.c
SRC_SHARED := $(filter-out $(SRC_NATIVE) $(SRC_WASM_ONLY),$(SRC_ALL))
SRC := $(SRC_SHARED) $(SRC_NATIVE)

TP  := third_party/yyjson/yyjson.c third_party/picohttpparser/picohttpparser.c \
       third_party/wslay/wslay_event.c third_party/wslay/wslay_frame.c \
       third_party/wslay/wslay_net.c third_party/wslay/wslay_queue.c \
       third_party/wslay/wslay_stack.c
# wslay + picohttpparser serve the native transports only; the wasm build
# keeps just yyjson so `nm` stays honest about dead code.
TP_WASM := third_party/yyjson/yyjson.c

REL_OBJS := $(SRC:%.c=$(OBJ_REL)/%.o) $(TP:%.c=$(OBJ_REL)/%.o)

TEST_SRC := $(wildcard tests/*.c)
TEST_DEPS := $(filter-out src/main.c,$(SRC)) $(TP)
TEST_OBJS := $(TEST_DEPS:%.c=$(OBJ_DBG)/%.o)

PREFIX ?= $(HOME)/.local

# Size budgets (docs/size-and-speed.md). Override SIZE_MAX in CI per target.
# 1.5 MiB Linux, 1.8 MiB Darwin, 2.0 MiB Windows (MSYS-linked).
ifeq ($(UNAME_S),Darwin)
  SIZE_MAX ?= 1887436
else ifeq ($(WINDOWS),1)
  SIZE_MAX ?= 2097152
else
  SIZE_MAX ?= 1572864
endif

.PHONY: all release debug test test-unit size size-check pack smoke bench clean install site FORCE

all: release

# Regenerated every run, rewritten only when the version changes, so cached
# objects survive; -MMD rebuilds the version's users when it does change.
$(VERSION_H): FORCE
	@mkdir -p $(@D)
	@printf '#define TNY_VERSION "%s"\n' '$(TNY_VERSION)' > $@.tmp
	@if cmp -s $@.tmp $@ 2>/dev/null; then rm -f $@.tmp; else mv $@.tmp $@; fi

FORCE:

release: $(BIN)

$(BIN): $(REL_OBJS)
	@mkdir -p $(@D)
	$(CC) $(REL_CFLAGS) -o $@ $^ $(REL_LDFLAGS)
	strip $@ 2>/dev/null || strip -x $@
	@wc -c $@

$(OBJ_REL)/%.o: %.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(REL_CFLAGS) -MMD -MP $(if $(findstring third_party,$<),-Wno-error -w,) -c -o $@ $<

$(OBJ_DBG)/%.o: %.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(DBG_CFLAGS) -MMD -MP $(if $(findstring third_party,$<),-Wno-error -w,) -c -o $@ $<

$(TEST_BIN): $(TEST_OBJS) $(TEST_SRC:%.c=$(OBJ_DBG)/%.o)
	@mkdir -p $(@D)
	$(CC) $(DBG_CFLAGS) -o $@ $^ $(DBG_LDFLAGS)

debug: $(TEST_BIN)

test-unit: $(TEST_BIN)
	./$(TEST_BIN)

test: test-unit release
	@if [ -x tests/integration/run.sh ]; then tests/integration/run.sh; fi

size: release
	@wc -c $(BIN)

# Fail if the stripped binary exceeds SIZE_MAX (bytes).
size-check: release
	@bytes=$$(wc -c < $(BIN) | tr -d ' '); \
	echo "$$bytes $(BIN) (limit $(SIZE_MAX))"; \
	if [ "$$bytes" -gt "$(SIZE_MAX)" ]; then \
		echo "error: $(BIN) is $$bytes bytes, over the $(SIZE_MAX)-byte budget" >&2; \
		exit 1; \
	fi

# Copy the stripped binary to dist/tny-<triple>[.exe]. TRIPLE is required.
pack: release
	@test -n "$(TRIPLE)" || { echo "error: pack needs TRIPLE=os-arch" >&2; exit 1; }
	@mkdir -p dist
	cp $(BIN) dist/tny-$(TRIPLE)$(EXE)
	@wc -c dist/tny-$(TRIPLE)$(EXE)

smoke: release
	./$(BIN) --version
	./$(BIN) --help >/dev/null
	./$(BIN) ask --help >/dev/null
	./$(BIN) doctor --json >/dev/null

bench: release
	hyperfine --warmup 5 -N './$(BIN) --version'

install: release
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(BIN) $(DESTDIR)$(PREFIX)/bin/tny$(EXE)

site:
	python3 scripts/site_build.py

# ---- wasm (docs/adr/0017): the same sources, the browser/node seams ----
# Two links from one object set: tny.js (node, NODERAWFS — what CI drives
# through the existing integration suite) and tny-web.mjs (browser, MEMFS —
# what the landing page loads). The C is identical; only FS + env glue differ.
EMCC        ?= emcc
OBJ_WASM     = $(BUILD)/wasm/obj
WASM_NODE    = $(BUILD)/wasm/tny.js
WASM_WEB     = $(BUILD)/wasm/tny-web.mjs
WASM_SRC    := $(SRC_SHARED) $(SRC_WASM_ONLY) $(TP_WASM)
WASM_OBJS   := $(WASM_SRC:%.c=$(OBJ_WASM)/%.o)
WASM_CFLAGS  = $(STD) $(WARN) $(INC) $(DEFS) -Os
# Asyncify is the suspension mechanism (JSPI is Chrome-only, COOP/COEP for
# workers cannot be set on GitHub Pages). Broad instrumentation first; narrow
# later if the size budget demands it (docs/adr/0017 footguns).
WASM_LDFLAGS = -Os -sASYNCIFY -sASYNCIFY_STACK_SIZE=131072 \
               -sALLOW_MEMORY_GROWTH -sEXIT_RUNTIME=1 -sSTACK_SIZE=1048576

$(OBJ_WASM)/%.o: %.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(EMCC) $(WASM_CFLAGS) -MMD -MP $(if $(findstring third_party,$<),-Wno-error -w,) -c -o $@ $<

$(WASM_NODE): $(WASM_OBJS) src/wasm/pre_node.js
	@mkdir -p $(@D)
	$(EMCC) $(WASM_LDFLAGS) -sENVIRONMENT=node -sNODERAWFS \
		--pre-js src/wasm/pre_node.js -o $@ $(WASM_OBJS)
	@printf '#!/bin/sh\nexec node "%s" "$$@"\n' "$$(cd $(@D) && pwd)/tny.js" > $(@D)/tny
	@chmod +x $(@D)/tny
	@wc -c $@ $(@:.js=.wasm)

$(WASM_WEB): $(WASM_OBJS) src/wasm/pre_web.js
	@mkdir -p $(@D)
	$(EMCC) $(WASM_LDFLAGS) -sENVIRONMENT=web -sMODULARIZE -sEXPORT_ES6 \
		-sINVOKE_RUN=0 -sEXPORTED_RUNTIME_METHODS=callMain,FS,ENV \
		--pre-js src/wasm/pre_web.js -o $@ $(WASM_OBJS)
	@wc -c $@ $(@:.mjs=.wasm)

wasm: $(WASM_NODE)
wasm-web: $(WASM_WEB)

# wasm size budget: artifact (js glue + wasm) stays under the Linux native
# budget so the browser build cannot quietly outgrow the product invariant.
WASM_SIZE_MAX ?= 1572864
wasm-size-check: wasm
	@bytes=$$(cat $(WASM_NODE) $(WASM_NODE:.js=.wasm) | wc -c | tr -d ' '); \
	echo "$$bytes wasm artifact (limit $(WASM_SIZE_MAX))"; \
	if [ "$$bytes" -gt "$(WASM_SIZE_MAX)" ]; then \
		echo "error: wasm artifact is $$bytes bytes, over the $(WASM_SIZE_MAX)-byte budget" >&2; \
		exit 1; \
	fi

.PHONY: wasm wasm-web wasm-size-check

clean:
	rm -rf $(BUILD) dist

# Header dependencies emitted by -MMD; a header edit rebuilds its users.
-include $(REL_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(TEST_SRC:%.c=$(OBJ_DBG)/%.d)
