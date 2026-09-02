# tny — C11 TUI + CLI coding-agent harness.
# Targets: make (release), make debug, make test, make size, make size-check,
#          make pack, make bench, make install, make site

CC      ?= cc
GIT     ?= git
BASH    ?= bash
ZSH     ?= zsh
STD      = -std=c11
WARN     = -Wall -Wextra -Werror -Wno-deprecated-declarations
INC      = -Iinclude -Isrc -Ithird_party -Ithird_party/yyjson -Ithird_party/picohttpparser \
           -Ithird_party/wslay -Ithird_party/wslay/wslay -Ithird_party/greatest
DEFS     = -DHAVE_ARPA_INET_H -DHAVE_NETINET_IN_H -D_DARWIN_C_SOURCE \
           -D_DEFAULT_SOURCE -D_BSD_SOURCE \
           -DTNY_SHELL_PATH=\"$(TNY_SHELL_PATH)\"
TNY_SHELL_PATH ?= /bin/sh

UNAME_S := $(shell uname -s 2>/dev/null || echo unknown)
UNAME_M := $(shell uname -m 2>/dev/null || echo unknown)
ifeq ($(UNAME_S),Darwin)
  # macOS dyld strips sanitizer insertion variables before Python can spawn
  # children. Prefer the framework's real app executable (not its launcher),
  # while allowing CI/toolchain callers to override discovery explicitly.
  DARWIN_PYTHON_APPS := $(wildcard \
    /opt/homebrew/opt/python@*/Frameworks/Python.framework/Versions/*/Resources/Python.app/Contents/MacOS/Python \
    /usr/local/opt/python@*/Frameworks/Python.framework/Versions/*/Resources/Python.app/Contents/MacOS/Python)
  SANITIZER_PYTHON ?= $(or $(firstword $(DARWIN_PYTHON_APPS)),\
    $(shell candidate="$$(python3 -c 'import sys; print(sys.prefix + "/Resources/Python.app/Contents/MacOS/Python")' 2>/dev/null)"; if test -x "$$candidate"; then printf '%s' "$$candidate"; else command -v python3; fi))
endif

# Version comes from git at build time (docs/adr/0014): the nearest v* tag,
# plus -N-g<hash>[-dirty] between releases. Release CI overrides it with the
# pushed tag (TNY_VERSION env/arg) so shallow and containerized builds do not
# depend on tag fetching. Tarball builds without git fall back below.
TNY_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null | sed 's/^v//')
ifeq ($(strip $(TNY_VERSION)),)
  TNY_VERSION := 0.0.0-unknown
endif
ifeq ($(UNAME_S),Darwin)
  LIBTNY_MACH_CURRENT_VERSION := $(shell python3 scripts/check_abi_baseline.py \
    --mach-version '$(TNY_VERSION)' --development-fallback 2>/dev/null)
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

SRC_PUBLIC_API := $(wildcard src/lib/*.c)
SRC_ALL := $(wildcard src/*.c src/util/*.c src/json/*.c src/core/*.c src/cli/*.c \
        src/net/*.c src/mcp/*.c src/tui/*.c \
        src/backends/openai/*.c src/backends/acp/*.c src/backends/codex/*.c \
        src/backends/cursor/*.c) src/lib/host_services.c src/lib/custom_tools.c

# Per-platform source lists (docs/adr/0017). Native transports (sockets, TLS,
# hand-rolled HTTP/1.1 + wslay WebSocket) and the poll(2) wrapper are excluded
# from the wasm build wholesale rather than #ifdef-riddled; src/net/net_wasm.c
# replaces the whole seam there (fetch, browser WebSocket, pseudo-fd registry).
SRC_NATIVE := src/net/tcp.c src/net/stream.c src/net/http1.c src/net/http_server.c src/net/ws.c \
              src/util/tny_poll.c src/backends/cursor/callbacks.c
SRC_WASM_ONLY := src/net/net_wasm.c src/backends/cursor/callbacks_wasm.c
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

# libtny ABI 1: headless runtime only. ACP server/turn are application
# adapters; the ACP client wire remains a library backend.
LIB_APP_EXCLUDE := src/main.c $(wildcard src/cli/*.c src/tui/*.c) \
                   src/backends/acp/acp_server.c src/backends/acp/acp_turn.c
LIB_SRC := $(SRC_PUBLIC_API) \
           $(filter-out $(LIB_APP_EXCLUDE) $(SRC_PUBLIC_API),$(SRC_SHARED)) \
           $(SRC_NATIVE)
OBJ_PIC := $(BUILD)/pic
LIB_PIC_OBJS := $(LIB_SRC:%.c=$(OBJ_PIC)/%.o) $(TP:%.c=$(OBJ_PIC)/%.o)
PIC_CFLAGS := $(REL_CFLAGS) -fPIC -fvisibility=hidden \
              -DTNY_SHARED_LIBRARY_BUILD=1 \
              -include src/util/alloc_override.h
OBJ_FAULT_PIC := $(BUILD)/fault-pic
FAULT_PIC_OBJS := $(LIB_SRC:%.c=$(OBJ_FAULT_PIC)/%.o) \
                  $(TP:%.c=$(OBJ_FAULT_PIC)/%.o)
FAULT_PIC_CFLAGS := $(PIC_CFLAGS) -DTNY_ALLOC_TESTING=1
OBJ_FAULT_SAN_PIC := $(BUILD)/fault-san-pic
FAULT_SAN_PIC_OBJS := $(LIB_SRC:%.c=$(OBJ_FAULT_SAN_PIC)/%.o) \
                      $(TP:%.c=$(OBJ_FAULT_SAN_PIC)/%.o)
FAULT_SAN_PIC_CFLAGS := $(FAULT_PIC_CFLAGS) -O1 -g \
                        -fsanitize=address,undefined \
                        -fno-omit-frame-pointer
OBJ_TSAN_PIC := $(BUILD)/tsan-pic
TSAN_PIC_OBJS := $(LIB_SRC:%.c=$(OBJ_TSAN_PIC)/%.o) \
                 $(TP:%.c=$(OBJ_TSAN_PIC)/%.o)
TSAN_PIC_CFLAGS := $(PIC_CFLAGS) -O1 -g -fsanitize=thread \
                   -fno-omit-frame-pointer
FUZZ_CC ?= clang
OBJ_FUZZ := $(BUILD)/fuzz-libfuzzer/obj
FUZZ_OBJS := $(LIB_SRC:%.c=$(OBJ_FUZZ)/%.o) \
             $(TP:%.c=$(OBJ_FUZZ)/%.o)
FUZZ_CFLAGS := $(PIC_CFLAGS) -O1 -g -fno-omit-frame-pointer \
               -fsanitize=fuzzer-no-link,address,undefined
FUZZ_HARNESS_CFLAGS := $(PIC_CFLAGS) -O1 -g -fno-omit-frame-pointer \
                       -fsanitize=fuzzer,address,undefined
FUZZ_SMOKE_BIN := $(BUILD)/fuzz/libtny-fuzz-smoke
FUZZ_BIN := $(BUILD)/fuzz-libfuzzer/libtny-fuzz
FUZZ_CORPUS := $(wildcard tests/fuzz/corpus-v1/*)
FUZZ_RUNS ?= 10000
FUZZ_SECONDS ?= 30
SAN_HOST := $(BUILD)/fault-san/libtny-sanitizer-host
ABI0_COMPAT_COMMIT := 510a95c2ef89aa9ec02a66d8b0a5cadd953025a8
ABI0_COMPAT_ARCHIVE ?=
ABI0_COMPAT_ARCHIVE_SHA256 := 8718336dbde47f3f8427bf6b3a724127e3ed24b61eaedb6f315523ec2a00c2f6
ABI0_COMPAT_ROOT := $(BUILD)/compat0/$(ABI0_COMPAT_COMMIT)
ABI0_COMPAT_SRC := $(ABI0_COMPAT_ROOT)/src
ABI0_COMPAT_BUILD := $(abspath $(ABI0_COMPAT_ROOT)/out)
ABI0_COMPAT_STAMP := $(ABI0_COMPAT_ROOT)/.source-verified
ifeq ($(UNAME_S),Darwin)
  LIB_REAL := $(BUILD)/lib/libtny.1.dylib
  LIB_LINK := $(BUILD)/lib/libtny.dylib
  LIB_COMPAT0_REAL := $(BUILD)/lib/libtny.0.dylib
  ABI0_COMPAT_BUILT := $(ABI0_COMPAT_BUILD)/lib/libtny.0.dylib
  LIB_LDFLAGS := -dynamiclib -Wl,-install_name,@rpath/libtny.1.dylib \
                 -Wl,-compatibility_version,1.0.0 \
                 -Wl,-current_version,$(LIBTNY_MACH_CURRENT_VERSION) \
                 -Wl,-dead_strip \
                 -Wl,-exported_symbols_list,abi/libtny.exports.macos
  LIB_EXPORT_FILE := abi/libtny.exports.macos
  LIB_FAULT_REAL := $(BUILD)/lib-fault/libtny.1.dylib
  LIB_FAULT_LINK := $(BUILD)/lib-fault/libtny.dylib
  LIB_FAULT_SAN_REAL := $(BUILD)/lib-fault-san/libtny.1.dylib
  LIB_FAULT_SAN_LINK := $(BUILD)/lib-fault-san/libtny.dylib
  LIB_FAULT_LDFLAGS := -dynamiclib \
                       -Wl,-install_name,@rpath/libtny.1.dylib \
                       -Wl,-compatibility_version,1.0.0 \
                       -Wl,-current_version,$(LIBTNY_MACH_CURRENT_VERSION) \
                       -Wl,-dead_strip
  ABI0_COMPAT_LIBS := -ltny.0
else
  LIB_REAL := $(BUILD)/lib/libtny.so.1
  LIB_LINK := $(BUILD)/lib/libtny.so
  LIB_COMPAT0_REAL := $(BUILD)/lib/libtny.so.0
  ABI0_COMPAT_BUILT := $(ABI0_COMPAT_BUILD)/lib/libtny.so.0
  LIB_LDFLAGS := -shared -Wl,-soname,libtny.so.1 -Wl,--gc-sections \
                 -Wl,--version-script,abi/libtny.map -pthread -ldl
  LIB_EXPORT_FILE := abi/libtny.map
  LIB_FAULT_REAL := $(BUILD)/lib-fault/libtny.so.1
  LIB_FAULT_LINK := $(BUILD)/lib-fault/libtny.so
  LIB_FAULT_SAN_REAL := $(BUILD)/lib-fault-san/libtny.so.1
  LIB_FAULT_SAN_LINK := $(BUILD)/lib-fault-san/libtny.so
  LIB_FAULT_LDFLAGS := -shared -Wl,-soname,libtny.so.1 \
                       -Wl,--gc-sections -pthread -ldl
  LIB_TSAN_REAL := $(BUILD)/lib-tsan/libtny.so.1
  LIB_TSAN_LINK := $(BUILD)/lib-tsan/libtny.so
  TSAN_HOST := $(BUILD)/tsan/libtny-tsan-host
  TSAN_CUSTOM_HOST := $(BUILD)/tsan/libtny-custom-tools-tsan
  ABI0_COMPAT_LIBS := -l:libtny.so.0
endif

TEST_SRC := $(wildcard tests/*.c)
TEST_DEPS := $(filter-out src/main.c,$(SRC)) $(SRC_PUBLIC_API) $(TP)
TEST_OBJS := $(TEST_DEPS:%.c=$(OBJ_DBG)/%.o)

PREFIX ?= $(HOME)/.local

# ABI 1 shared artifacts are intentionally limited to the active baseline.
# CLI/wasm/static builds still compile shared internals but are not libtny
# packages and must not advertise shared/static linkage capabilities.
LIBTNY_SHARED_SUPPORTED := 0
ifeq ($(STATIC),0)
  ifeq ($(UNAME_S),Darwin)
    ifeq ($(UNAME_M),arm64)
      LIBTNY_SHARED_SUPPORTED := 1
    endif
  else ifeq ($(UNAME_S),Linux)
    ifneq ($(filter x86_64 aarch64 arm64,$(UNAME_M)),)
      ifneq ($(shell getconf GNU_LIBC_VERSION 2>/dev/null),)
        LIBTNY_SHARED_SUPPORTED := 1
      endif
    endif
  endif
endif

# Size budgets (docs/size-and-speed.md). Override SIZE_MAX in CI per target.
# 1.0 MiB Linux dynamic (docs/adr/0053 — no tmux, app stays light; musl
# static overrides to 1.5 MiB in CI), 1.8 MiB Darwin, 2.0 MiB Windows
# (MSYS-linked).
ifeq ($(UNAME_S),Darwin)
  SIZE_MAX ?= 1887436
else ifeq ($(WINDOWS),1)
  SIZE_MAX ?= 2097152
else
  SIZE_MAX ?= 1048576
endif

.PHONY: all release debug test test-unit test-event-schema test-conformance-contract check-cursor-sdk-contract test-cursor-sdk-contract test-extensions-python test-shell-workflows test-install-prefix test-abi test-sdk-python test-sdk-typescript test-sdks test-libtny-fault test-libtny-fault-sanitize test-libtny-tsan test-libtny-mutation test-libtny-fuzz-smoke test-libtny-fuzz size size-check pack smoke bench clean install install-lib install-lib-active lib-shared lib-shared-active lib-shared-compat0 lib-shared-fault lib-shared-fault-sanitize lib-shared-tsan site FORCE

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

$(OBJ_PIC)/%.o: %.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(if $(or $(findstring third_party,$<),$(findstring src/util/alloc.c,$<)),$(filter-out -include src/util/alloc_override.h,$(PIC_CFLAGS)) $(if $(findstring third_party,$<),-Wno-error -w,),$(PIC_CFLAGS)) -MMD -MP -c -o $@ $<

$(OBJ_FAULT_PIC)/%.o: %.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(if $(or $(findstring third_party,$<),$(findstring src/util/alloc.c,$<)),$(filter-out -include src/util/alloc_override.h,$(FAULT_PIC_CFLAGS)) $(if $(findstring third_party,$<),-Wno-error -w,),$(FAULT_PIC_CFLAGS)) -MMD -MP -c -o $@ $<

$(OBJ_FAULT_SAN_PIC)/%.o: %.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(if $(or $(findstring third_party,$<),$(findstring src/util/alloc.c,$<)),$(filter-out -include src/util/alloc_override.h,$(FAULT_SAN_PIC_CFLAGS)) $(if $(findstring third_party,$<),-Wno-error -w,),$(FAULT_SAN_PIC_CFLAGS)) -MMD -MP -c -o $@ $<

$(OBJ_TSAN_PIC)/%.o: %.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(if $(or $(findstring third_party,$<),$(findstring src/util/alloc.c,$<)),$(filter-out -include src/util/alloc_override.h,$(TSAN_PIC_CFLAGS)) $(if $(findstring third_party,$<),-Wno-error -w,),$(TSAN_PIC_CFLAGS)) -MMD -MP -c -o $@ $<

$(OBJ_FUZZ)/%.o: %.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(if $(or $(findstring third_party,$<),$(findstring src/util/alloc.c,$<)),$(filter-out -include src/util/alloc_override.h,$(FUZZ_CFLAGS)) $(if $(findstring third_party,$<),-Wno-error -w,),$(FUZZ_CFLAGS)) -MMD -MP -c -o $@ $<

ifeq ($(LIBTNY_SHARED_SUPPORTED),1)
lib-shared-active: $(LIB_LINK)
lib-shared: lib-shared-active $(LIB_COMPAT0_REAL)
lib-shared-compat0: $(LIB_COMPAT0_REAL)
else
lib-shared lib-shared-active lib-shared-compat0:
	@echo "error: ABI 1 shared libtny is supported only on macOS arm64 and glibc Linux x86_64/aarch64 dynamic builds" >&2
	@exit 2
endif

lib-shared-fault: $(LIB_FAULT_LINK)

lib-shared-fault-sanitize: $(LIB_FAULT_SAN_LINK)

ifeq ($(UNAME_S),Linux)
lib-shared-tsan: $(LIB_TSAN_LINK)
else
lib-shared-tsan:
	@echo "error: libtny TSan is supported only by the Linux compiler lane" >&2
	@exit 2
endif

$(LIB_REAL): $(LIB_PIC_OBJS) $(LIB_EXPORT_FILE)
	@mkdir -p $(@D)
	@test -n "$(if $(filter Darwin,$(UNAME_S)),$(LIBTNY_MACH_CURRENT_VERSION),ok)" || \
		python3 scripts/check_abi_baseline.py --mach-version '$(TNY_VERSION)' \
			--development-fallback
	$(CC) -o $@ $(LIB_PIC_OBJS) $(LIB_LDFLAGS)

$(LIB_LINK): $(LIB_REAL)
	@mkdir -p $(@D)
	@cd $(@D) && ln -sf $(notdir $(LIB_REAL)) $(notdir $@)

$(ABI0_COMPAT_STAMP): abi/compat0.json scripts/check_abi_baseline.py
	@mkdir -p $(ABI0_COMPAT_SRC)
	@if $(GIT) cat-file -e $(ABI0_COMPAT_COMMIT)^{commit} 2>/dev/null; then \
		$(GIT) archive $(ABI0_COMPAT_COMMIT) | tar -x -C $(ABI0_COMPAT_SRC); \
	elif test -n "$(ABI0_COMPAT_ARCHIVE)" && \
		test -f "$(ABI0_COMPAT_ARCHIVE)"; then \
		actual=$$(shasum -a 256 "$(ABI0_COMPAT_ARCHIVE)" | awk '{print $$1}'); \
		test "$$actual" = "$(ABI0_COMPAT_ARCHIVE_SHA256)" || { \
			echo "error: ABI0_COMPAT_ARCHIVE hash mismatch" >&2; exit 1; }; \
		tar -xf "$(ABI0_COMPAT_ARCHIVE)" -C $(ABI0_COMPAT_SRC); \
	else \
		echo "error: frozen ABI0 commit unavailable; use a full checkout or set ABI0_COMPAT_ARCHIVE to the verified git archive" >&2; \
		exit 1; \
	fi
	python3 scripts/check_abi_baseline.py --compat0 abi/compat0.json \
		--compat0-source-root $(ABI0_COMPAT_SRC)
	@touch $@

$(LIB_COMPAT0_REAL): $(ABI0_COMPAT_STAMP)
	$(MAKE) -C $(ABI0_COMPAT_SRC) BUILD=$(ABI0_COMPAT_BUILD) \
		CC='$(CC)' TNY_VERSION='$(TNY_VERSION)' lib-shared
	@mkdir -p $(@D)
	cp $(ABI0_COMPAT_BUILT) $@

$(LIB_FAULT_REAL): $(FAULT_PIC_OBJS) $(LIB_EXPORT_FILE)
	@mkdir -p $(@D)
	$(CC) -o $@ $(FAULT_PIC_OBJS) $(LIB_FAULT_LDFLAGS)

$(LIB_FAULT_LINK): $(LIB_FAULT_REAL)
	@mkdir -p $(@D)
	@cd $(@D) && ln -sf $(notdir $(LIB_FAULT_REAL)) $(notdir $@)

$(LIB_FAULT_SAN_REAL): $(FAULT_SAN_PIC_OBJS) $(LIB_EXPORT_FILE)
	@mkdir -p $(@D)
	$(CC) -o $@ $(FAULT_SAN_PIC_OBJS) $(LIB_FAULT_LDFLAGS) \
		-fsanitize=address,undefined

$(LIB_FAULT_SAN_LINK): $(LIB_FAULT_SAN_REAL)
	@mkdir -p $(@D)
	@cd $(@D) && ln -sf $(notdir $(LIB_FAULT_SAN_REAL)) $(notdir $@)

$(SAN_HOST): tests/integration/libtny_sanitizer_host.c $(LIB_FAULT_SAN_REAL)
	@mkdir -p $(@D)
	$(CC) $(STD) $(WARN) -Iinclude -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined -o $@ $< $(LIB_FAULT_SAN_REAL) \
		$(if $(filter Linux,$(UNAME_S)),-pthread -ldl,) \
		-Wl,-rpath,$(abspath $(dir $(LIB_FAULT_SAN_REAL)))

ifeq ($(UNAME_S),Linux)
$(LIB_TSAN_REAL): $(TSAN_PIC_OBJS) $(LIB_EXPORT_FILE)
	@mkdir -p $(@D)
	$(CC) -o $@ $(TSAN_PIC_OBJS) $(LIB_LDFLAGS) -fsanitize=thread

$(LIB_TSAN_LINK): $(LIB_TSAN_REAL)
	@mkdir -p $(@D)
	@cd $(@D) && ln -sf $(notdir $(LIB_TSAN_REAL)) $(notdir $@)

$(TSAN_HOST): tests/integration/libtny_tsan_host.c $(LIB_TSAN_REAL)
	@mkdir -p $(@D)
	$(CC) $(STD) $(WARN) -Iinclude -O1 -g -fno-omit-frame-pointer \
		-fsanitize=thread -o $@ $< $(LIB_TSAN_REAL) -pthread -ldl \
		-Wl,-rpath,$(abspath $(dir $(LIB_TSAN_REAL)))

$(TSAN_CUSTOM_HOST): tests/integration/libtny_custom_tools.c $(LIB_TSAN_REAL)
	@mkdir -p $(@D)
	$(CC) $(STD) $(WARN) -Iinclude -O1 -g -fno-omit-frame-pointer \
		-fsanitize=thread -o $@ $< $(LIB_TSAN_REAL) -pthread -ldl \
		-Wl,-rpath,$(abspath $(dir $(LIB_TSAN_REAL)))
endif

$(TEST_BIN): $(TEST_OBJS) $(TEST_SRC:%.c=$(OBJ_DBG)/%.o)
	@mkdir -p $(@D)
	$(CC) $(DBG_CFLAGS) -o $@ $^ $(DBG_LDFLAGS)

debug: $(TEST_BIN)

test-unit: $(TEST_BIN) $(BIN)
	./$(TEST_BIN)

test-event-schema:
	python3 sdk/schema/check.py

test-conformance-contract:
	python3 sdk/conformance/check.py
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
		-s tests/conformance -p 'test_*.py' -v

check-cursor-sdk-contract:
	PYTHONDONTWRITEBYTECODE=1 python3 scripts/check_cursor_sdk_v1.py

test-cursor-sdk-contract: check-cursor-sdk-contract
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
		tests.integration.test_cursor_sdk_contract -v

test-extensions-python:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/extensions -p 'test_*.py' -v

test-shell-workflows:
	$(BASH) tests/shell/test_workflows.sh
	$(ZSH) tests/shell/test_workflows.sh

test-install-prefix: release
	PYTHONDONTWRITEBYTECODE=1 python3 tests/packaging/test_make_install.py

TNY ?= $(abspath $(BIN))
test-help-flags: release
	TNY="$(TNY)" python3 tests/integration/test_help_flags.py

test-abi: TNY_VERSION=1.0.0
test-abi: lib-shared
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
		-s tests/abi -p 'test_*.py' -v
	python3 scripts/check_abi_baseline.py --compat0 abi/compat0.json
	python3 scripts/check_abi_baseline.py \
		--candidate $(BUILD)/abi/libtny-v1-current.json
	python3 scripts/check_abi_baseline.py \
		--baseline abi/baseline-v1.0.json \
		--candidate $(BUILD)/abi/libtny-v1-current.json

# SDK tests intentionally stay outside `make test`: normal CLI/libtny builds
# require neither cffi nor Node.js. The dedicated SDK workflow installs them.
test-sdk-python: lib-shared
	PYTHONPATH=$(CURDIR)/sdk/python/src \
	TNY_TEST_LIBRARY=$(LIB_REAL) \
	python3 -m unittest discover -s sdk/python/tests -p 'test_*.py' -v
	mkdir -p $(BUILD)/conformance
	PYTHONPATH=$(CURDIR)/sdk/python/src \
	TNY_TEST_LIBRARY=$(LIB_REAL) \
	python3 sdk/conformance/run.py --artifact $(LIB_REAL) \
		--report $(BUILD)/conformance/python.json -- \
		python3 sdk/python/conformance_adapter.py

test-sdk-typescript: lib-shared
	npm --prefix sdk/typescript run build
	npm --prefix sdk/typescript test
	mkdir -p $(BUILD)/conformance
	python3 sdk/conformance/run.py \
		--artifact sdk/typescript/build/Release/tny.node \
		--report $(BUILD)/conformance/typescript.json -- \
		node sdk/typescript/test/conformance-adapter.mjs

test-sdks: test-sdk-python test-sdk-typescript

test-libtny-fault: lib-shared-fault
	python3 tests/integration/test_net_host_safety.py
	python3 tests/integration/test_libtny_faults.py $(LIB_FAULT_REAL)

test-libtny-mutation:
	python3 tests/mutation/mutate.py --focus libtny-safety
	python3 tests/mutation/mutate.py --focus libtny-fault-mutation
	python3 tests/mutation/mutate.py --focus libtny-custom-tools

$(FUZZ_SMOKE_BIN): tests/fuzz/fuzz_libtny.c $(sort $(TEST_OBJS))
	@mkdir -p $(@D)
	$(CC) $(DBG_CFLAGS) -DTNY_FUZZ_STANDALONE=1 -o $@ $< \
		$(sort $(TEST_OBJS)) $(DBG_LDFLAGS)

test-libtny-fuzz-smoke: $(FUZZ_SMOKE_BIN)
	$(FUZZ_SMOKE_BIN) --self-test
	@rc=0; $(FUZZ_SMOKE_BIN) --negative-self-test || rc=$$?; \
	if [ "$$rc" -ne 1 ]; then \
		echo "error: fuzz negative self-test returned $$rc, expected 1" >&2; \
		exit 1; \
	else \
		echo "fuzz negative self-test: expected missing-class rejection"; \
	fi
	$(FUZZ_SMOKE_BIN) $(FUZZ_CORPUS)

$(FUZZ_BIN): tests/fuzz/fuzz_libtny.c $(FUZZ_OBJS)
	@mkdir -p $(@D) $(BUILD)/fuzz-artifacts
	$(FUZZ_CC) $(FUZZ_HARNESS_CFLAGS) \
		-o $@ $< $(FUZZ_OBJS) \
		$(if $(filter Linux,$(UNAME_S)),-pthread -ldl,)

ifeq ($(UNAME_S)-$(UNAME_M),Linux-x86_64)
test-libtny-fuzz: $(FUZZ_BIN)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		$(FUZZ_BIN) -runs=$(FUZZ_RUNS) -max_total_time=$(FUZZ_SECONDS) \
		-timeout=5 -max_len=131072 -rss_limit_mb=1024 \
		-artifact_prefix=$(BUILD)/fuzz-artifacts/ tests/fuzz/corpus-v1
else
test-libtny-fuzz:
	@echo "error: libtny libFuzzer gate is supported only on Linux x86_64" >&2
	@exit 2
endif

test-libtny-fault-sanitize: lib-shared-fault-sanitize $(SAN_HOST)
ifeq ($(UNAME_S),Darwin)
	@runtime="$$($(CC) --print-resource-dir)/lib/darwin/libclang_rt.asan_osx_dynamic.dylib"; \
	python="$(SANITIZER_PYTHON)"; \
	test -f "$$runtime" || { echo "error: ASan runtime not found" >&2; exit 1; }; \
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	TNY_TEST_ASAN_RUNTIME="$$runtime" \
	TNY_TEST_PYTHON_EXEC="$$python" \
	DYLD_INSERT_LIBRARIES="$$runtime" \
	"$$python" tests/integration/test_libtny_faults.py $(LIB_FAULT_SAN_REAL)
else
	@runtime="$$($(CC) -print-file-name=libasan.so)"; \
	test -f "$$runtime" || { echo "error: ASan runtime not found" >&2; exit 1; }; \
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	TNY_TEST_ASAN_RUNTIME="$$runtime" \
	LD_PRELOAD="$$runtime" \
	python3 tests/integration/test_libtny_faults.py $(LIB_FAULT_SAN_REAL)
endif
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/integration/libtny_sanitizer_launcher.py $(SAN_HOST)

ifeq ($(UNAME_S),Linux)
test-libtny-tsan: $(TSAN_HOST) $(TSAN_CUSTOM_HOST)
	python3 tests/integration/libtny_tsan_launcher.py $(TSAN_HOST)
	TNY_CUSTOM_TOOL_HOST=$(TSAN_CUSTOM_HOST) \
		python3 tests/integration/test_libtny_custom_tools.py
else
test-libtny-tsan:
	@echo "error: libtny TSan verification is supported only on Linux" >&2
	@exit 2
endif

test: test-unit test-event-schema test-conformance-contract test-cursor-sdk-contract test-extensions-python test-install-prefix test-help-flags release
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
	mkdir -p "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/lib/tny/tny_ext" \
		"$(DESTDIR)$(PREFIX)/share/tny"
	# unlink first: macOS kills (SIGKILL) a code-signed Mach-O overwritten in place
	rm -f "$(DESTDIR)$(PREFIX)/bin/tny$(EXE)"
	cp "$(BIN)" "$(DESTDIR)$(PREFIX)/bin/tny$(EXE)"
	cp python/tny_extension_host.py "$(DESTDIR)$(PREFIX)/lib/tny/"
	cp python/tny_ext/*.py python/tny_ext/py.typed \
		"$(DESTDIR)$(PREFIX)/lib/tny/tny_ext/"
	cp shell/tny-workflows.sh "$(DESTDIR)$(PREFIX)/share/tny/"
	chmod 755 "$(DESTDIR)$(PREFIX)/share/tny/tny-workflows.sh"

install-lib-active: lib-shared-active
	mkdir -p "$(DESTDIR)$(PREFIX)/include/tny" \
		"$(DESTDIR)$(PREFIX)/lib/pkgconfig"
	cp include/tny/tny.h "$(DESTDIR)$(PREFIX)/include/tny/tny.h"
	cp -P $(LIB_REAL) $(LIB_LINK) "$(DESTDIR)$(PREFIX)/lib/"
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(TNY_VERSION)|g' \
		libtny.pc.in > \
		"$(DESTDIR)$(PREFIX)/lib/pkgconfig/libtny.pc"

install-lib: lib-shared
	mkdir -p "$(DESTDIR)$(PREFIX)/include/tny" \
		"$(DESTDIR)$(PREFIX)/include/tny-0/tny" \
		"$(DESTDIR)$(PREFIX)/lib/pkgconfig"
	cp include/tny/tny.h "$(DESTDIR)$(PREFIX)/include/tny/tny.h"
	cp $(ABI0_COMPAT_SRC)/include/tny/tny.h \
		"$(DESTDIR)$(PREFIX)/include/tny-0/tny/tny.h"
	cp -P $(LIB_REAL) $(LIB_LINK) $(LIB_COMPAT0_REAL) \
		"$(DESTDIR)$(PREFIX)/lib/"
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(TNY_VERSION)|g' \
		libtny.pc.in > \
		"$(DESTDIR)$(PREFIX)/lib/pkgconfig/libtny.pc"
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(TNY_VERSION)|g' \
		-e 's|@COMPAT_LIBS@|$(ABI0_COMPAT_LIBS)|g' libtny-0.pc.in > \
		"$(DESTDIR)$(PREFIX)/lib/pkgconfig/libtny-0.pc"

site:
	python3 scripts/site_build.py

# ---- quality gates (docs/adr/0039) --------------------------------------
# `make quality` is the authoritative first-party lint/format/analysis
# aggregate; CI runs it in a fast job before the platform matrix. Tool
# versions are pinned twice, in lockstep (docs/adr/0061): .mise.toml for
# developers and .github/workflows/ci.yml for CI, so
#   mise install && make quality
# needs no flags anywhere. Without mise, point the variables at uvx wrappers:
#   make quality CLANG_FORMAT='uvx clang-format@21.1.2' CLANG_TIDY='uvx clang-tidy@22.1.8'
CLANG_FORMAT ?= clang-format
CLANG_TIDY   ?= clang-tidy
RUFF         ?= ruff
SHELLCHECK   ?= shellcheck
SHFMT        ?= shfmt
ACTIONLINT   ?= actionlint
ANALYZER_CC  ?= gcc

# First-party scopes only; third_party/ and frozen ABI fixtures stay exempt.
# Derive these lists from the tracked tree so new source and shell files enter
# the quality gate automatically instead of depending on maintained globs.
# Tracked *and* untracked-but-not-ignored sources: a file in flight is
# exactly the one whose formatting has not been checked yet.
FMT_SRC := $(shell { git ls-files -- '*.c' '*.h'; \
	git ls-files --others --exclude-standard -- '*.c' '*.h'; } | sort -u | \
	grep -Ev '^(third_party/|tests/abi/fixtures/)')
SH_SRC  := $(shell git ls-files -- '*.sh')
SHFMT_FLAGS := -i 4 -ci -sr
JS_SRC  := docs/assets/site.js docs/assets/term-core.js docs/assets/term-wasm.js \
           $(wildcard site/assets/*.js src/wasm/*.js tests/site/*.js \
           sdk/typescript/examples/*.mjs sdk/typescript/scripts/*.mjs \
           sdk/typescript/test/*.mjs) \
           sdk/typescript/dist/index.mjs

# clang-tidy analyzes the native translation units with the release flag
# set (minus -Werror; WarningsAsErrors in .clang-tidy is the gate).
TIDY_SRC    := $(SRC) $(SRC_PUBLIC_API)
TIDY_CFLAGS  = $(STD) $(filter-out -Werror,$(WARN)) $(INC) $(DEFS)
ifeq ($(UNAME_S),Darwin)
  TIDY_CFLAGS += -isysroot $(shell xcrun --show-sdk-path)
endif

# Stricter first-party diagnostics than the build's $(WARN); suppressions:
# format-nonliteral (buf_appendf takes caller fmts), overlength-strings
# (embedded JSON tool schemas exceed the C99 4095 minimum).
WARN_STRICT = -Wpedantic -Wformat=2 -Wno-format-nonliteral -Wno-overlength-strings \
              -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wundef \
              -Wwrite-strings -Wvla

format:
	$(CLANG_FORMAT) -i $(FMT_SRC)
	$(RUFF) format .
	$(SHFMT) -w $(SHFMT_FLAGS) $(SH_SRC)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FMT_SRC)
	$(RUFF) format --check .
	$(SHFMT) -d $(SHFMT_FLAGS) $(SH_SRC)

tidy: $(VERSION_H)
	$(CLANG_TIDY) --quiet $(TIDY_SRC) -- $(TIDY_CFLAGS)

warn-strict: $(VERSION_H)
	$(CC) $(REL_CFLAGS) $(WARN_STRICT) -fsyntax-only $(SRC) $(SRC_PUBLIC_API)

# GCC's path-sensitive analyzer (leaks, use-after-free, fd/stream misuse).
# Complementary to clang-tidy; Linux CI runs it, gcc is required.
# double-free is off: it misreads the oom-flag-guarded free in buf_detach
# (src/util/util.c) and flags every caller of path_join.
analyze: $(VERSION_H)
	@for f in $(SRC) $(SRC_PUBLIC_API); do \
		$(ANALYZER_CC) $(STD) $(WARN) $(INC) $(DEFS) -fanalyzer -O1 \
			-Wno-analyzer-double-free \
			-c -o /dev/null $$f || exit 1; \
	done
	@echo "analyze: $(words $(SRC) $(SRC_PUBLIC_API)) files clean"

lint-py:
	$(RUFF) check .

lint-sh:
	$(SHELLCHECK) $(SH_SRC)

lint-workflows:
	$(ACTIONLINT)

lint-js:
	@for f in $(JS_SRC); do node --check $$f || exit 1; done
	@echo "lint-js: $(words $(JS_SRC)) files clean"

ifeq ($(UNAME_S),Linux)
  QUALITY_ANALYZE := analyze
else
  QUALITY_ANALYZE :=
endif

quality: check-cursor-sdk-contract format-check tidy warn-strict lint-py lint-sh lint-workflows lint-js $(QUALITY_ANALYZE)
	@if [ -z "$(QUALITY_ANALYZE)" ]; then \
		echo "quality: GCC -fanalyzer skipped on $(UNAME_S); CI runs it on Linux"; \
	fi

.PHONY: format format-check tidy warn-strict analyze lint-py lint-sh lint-workflows lint-js quality

# ---- leak checks (docs/adr/0061) ----------------------------------------
# ASan/UBSan is the default test build, and neither checker can see through
# it: both replace malloc. So the leak gate rebuilds the same sources with
# SANITIZE=0 into $(LEAK_BUILD) and runs them under the host's checker —
# valgrind on Linux, /usr/bin/leaks on macOS (valgrind has no arm64 Darwin
# port), an honest skip elsewhere. scripts/leakcheck.sh is the driver.
LEAK_BUILD     = $(BUILD)/leakcheck
LEAK_TEST_BIN  = $(LEAK_BUILD)/tny-test$(EXE)
LEAK_CLI_BIN   = $(LEAK_BUILD)/tny$(EXE)
VALGRIND      ?= valgrind
# --child-silent-after-fork: several suites fork, and a child that exits
# mid-test reports the parent's still-live heap as lost. Only the parent's
# report is the truth. --errors-for-leak-kinds: "possibly lost" here is
# glibc's per-thread stack/DTV for threads alive at exit, never a first-party
# leak; definite and indirect losses are what fail the build.
VALGRIND_FLAGS ?= --leak-check=full --error-exitcode=1 \
                  --child-silent-after-fork=yes \
                  --errors-for-leak-kinds=definite,indirect \
                  --suppressions=tests/valgrind.supp
LEAKS         ?= leaks

# `leaks --atExit` installs an exit hook that stops the process for analysis,
# and fork(2) copies it into every child: a suite that spawns a helper
# deadlocks the run, and MallocStackLogging's banner corrupts the stdout the
# tests read back. macOS therefore runs suite by suite and skips the
# process-spawning suites, which valgrind still covers on Linux.
# runner_suite joined them with the session control channel (docs/adr/0058):
# its correlation tests fork a runner and a terminal child.
LEAK_SUITE_SKIP := cursor_suite cursor_sdk_suite mcp_suite runner_suite \
	session_bg_suite ssh_suite
LEAK_SUITES := $(filter-out $(LEAK_SUITE_SKIP),\
	$(shell sed -n 's/.*RUN_SUITE(\([A-Za-z0-9_]*\)).*/\1/p' tests/test_main.c))

LEAK_ENV = TEST_BIN=$(LEAK_TEST_BIN) CLI_BIN=$(LEAK_CLI_BIN) \
	VALGRIND='$(VALGRIND)' VALGRIND_FLAGS='$(VALGRIND_FLAGS)' \
	LEAKS='$(LEAKS)' LEAK_SUITES='$(LEAK_SUITES)'

leak-build:
	$(MAKE) SANITIZE=0 BUILD=$(LEAK_BUILD) debug release

leaks: leak-build
	@$(LEAK_ENV) $(BASH) scripts/leakcheck.sh auto

ifeq ($(UNAME_S),Linux)
valgrind: leak-build
	@$(LEAK_ENV) $(BASH) scripts/leakcheck.sh valgrind
else
valgrind:
	@echo "error: make valgrind is Linux-only; use make leaks (macOS) or make leaks-docker" >&2
	@exit 2
endif

# The valgrind flavour from a non-Linux host: a throwaway Linux container
# builds and checks a copy of the tree, so nothing root-owned lands in the
# working copy. Override LEAK_DOCKER_IMAGE to reuse a prebuilt toolchain.
LEAK_DOCKER_IMAGE ?= ubuntu:24.04
LEAK_DOCKER_SETUP ?= apt-get update -qq && \
	DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
	build-essential valgrind git python3 ca-certificates
leaks-docker:
	@command -v docker > /dev/null 2>&1 || { \
		echo "error: docker not found; install it or run make leaks" >&2; \
		exit 2; \
	}
	docker run --rm --init -v "$(CURDIR):/src:ro" -w /work \
		$(LEAK_DOCKER_IMAGE) sh -euc \
		'cp -a /src/. /work && rm -rf /work/build; \
		 test -f /work/Makefile || { \
		   echo "error: /src is empty — your docker file sharing does not cover $(CURDIR) (colima and Docker Desktop share $$HOME by default)" >&2; \
		   exit 2; \
		 }; \
		 $(LEAK_DOCKER_SETUP) && make valgrind TNY_VERSION=$(TNY_VERSION)'

.PHONY: leak-build leaks valgrind leaks-docker

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

# ---- monorepo siblings (docs/adr/0045) ----------------------------------
# Each sibling app owns its own Makefile; these targets only delegate.
tnytty:
	$(MAKE) -C tnytty
tnytty-test:
	$(MAKE) -C tnytty test
tnytty-clean:
	$(MAKE) -C tnytty clean
.PHONY: tnytty tnytty-test tnytty-clean

# Header dependencies emitted by -MMD; a header edit rebuilds its users.
-include $(REL_OBJS:.o=.d) $(LIB_PIC_OBJS:.o=.d) \
         $(FAULT_PIC_OBJS:.o=.d) $(FAULT_SAN_PIC_OBJS:.o=.d) \
         $(TSAN_PIC_OBJS:.o=.d) $(FUZZ_OBJS:.o=.d) $(TEST_OBJS:.o=.d) \
         $(TEST_SRC:%.c=$(OBJ_DBG)/%.d)
