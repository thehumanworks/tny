# tny — C11 + private C++20 TUI + CLI coding-agent harness.
# Targets: make (release), make debug, make test, make size, make size-check,
#          make pack, make bench, make install, make site

CC      ?= cc
# Explicit cross/wrapper toolchains should set both drivers (CC and CXX).
# Derive the ordinary cc/gcc/clang pair, including versioned GCC/Clang.
ifneq ($(filter default undefined,$(origin CXX)),)
  CXX = $(call cxx_driver,$(CC))
endif
CXX     ?= c++
cxx_name = $(patsubst %cc,%c++,$(subst gcc,g++,$(subst clang,clang++,$(1))))
cxx_driver = $(foreach arg,$(1),$(subst $(notdir $(arg)),$(call cxx_name,$(notdir $(arg))),$(arg)))
CXXSTD   = -std=c++20
# Owners use the explicit tny allocator; C allocation macros poison STL headers.
# Vendor VERSION files can shadow <version> on case-insensitive filesystems.
# Vendors remain available for quoted/angle includes, after the standard library;
# first-party include paths and their diagnostics retain normal precedence.
CXX_GLIBC_FLOOR := $(if $(wildcard src/util/cxx_glibc_floor.h),-include src/util/cxx_glibc_floor.h)
cxx_flags = $(subst -Ithird_party,-idirafter third_party,$(filter-out $(STD),$(subst -include src/util/alloc_override.h,,$(1)))) $(CXXSTD) -fexceptions -fno-rtti $(CXX_GLIBC_FLOOR)
# Keep foo.c and foo.cpp distinct: C adapters can coexist with their owners.
objects = $(addprefix $(1)/,$(patsubst %.cpp,%.cpp.o,$(patsubst %.c,%.o,$(2))))
GIT     ?= git
BASH    ?= bash
ZSH     ?= zsh
TMUX_BIN ?= tmux
STD      = -std=c11
WARN     = -Wall -Wextra -Werror -Wno-deprecated-declarations
INC      = -Iinclude -Isrc -Ithird_party -Ithird_party/yyjson -Ithird_party/picohttpparser \
           -Ithird_party/greatest
DEFS     = -DHAVE_ARPA_INET_H -DHAVE_NETINET_IN_H -D_DARWIN_C_SOURCE \
           -D_DEFAULT_SOURCE -D_BSD_SOURCE \
           -DTNY_SHELL_PATH=\"$(TNY_SHELL_PATH)\"
# tny never sets a YYJSON_READ_ALLOW_*/WRITE_ALLOW_* flag, so the vendored
# reader's and writer's non-standard JSON paths (comments, NaN/Inf, trailing
# commas, invalid unicode) are dead code; the knob is documented upstream.
DEFS    += -DYYJSON_DISABLE_NON_STANDARD
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
  LIBTNY_MACH_CURRENT_VERSION ?= $(shell python3 scripts/check_abi_baseline.py \
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
  # MSYS2/Cygwin build a static yyjson dependency into the executable, not a
  # yyjson DLL. Their POSIX compiler does not define _WIN32; use yyjson's
  # supported API-annotation override rather than ELF visibility attributes,
  # which GCC LTO diagnoses again at link time under our unchanged -Werror.
  DEFS += -Dyyjson_api=
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
# Native executable objects only (ADR0092); never inherited by PIC/debug/wasm.
REL_LTO = -flto
# GCC's automatic LTO scheduling avoids its serial-LTRANS warning without
# disabling diagnostics; Clang retains its supported native LTO spelling.
ifeq (,$(findstring clang,$(shell $(CC) --version 2>/dev/null)))
  REL_LTO = -flto=auto
endif
# Let -Os/LTO choose JSON helper inlining for native releases (ADR0100).
# Kept out of REL_CFLAGS, which also feeds PIC/library and analysis builds.
REL_INLINE = -Dyyjson_inline=inline
# Linux native executables omit the frame pointer (ADR0111): GCC keeps it
# on aarch64 at every -O level, which costs ~9 KiB of prologues and, with
# the 64 KiB LOAD alignment plus RELRO, can add a whole page to the file.
# Unwind tables stay (backtraces come from .eh_frame, not x29). Darwin
# arm64 requires frame pointers by ABI and is left alone.
# Linux Clang uses -Oz to keep the native executable small (ADR0102).
# Probe the selected command, including wrappers; leave other build lanes alone.
REL_SIZE_OPT =
ifeq ($(UNAME_S),Linux)
  REL_SIZE_OPT = -fomit-frame-pointer -momit-leaf-frame-pointer
  ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
    REL_SIZE_OPT += -Oz
  endif
endif
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
  REL_LDFLAGS += -static-libgcc -static-libstdc++
endif

BUILD    = build
OBJ_REL  = $(BUILD)/rel
OBJ_DBG  = $(BUILD)/dbg
GEN      = $(BUILD)/generated
VERSION_H = $(GEN)/tny_version.h
INC     += -I$(GEN)

CPP_SRC := $(wildcard src/util/*.cpp src/json/*.cpp src/net/*.cpp \
                     src/backends/openai/*.cpp src/core/*.cpp src/lib/*.cpp)
SRC_PUBLIC_API := $(wildcard src/lib/*.c)
C_SRC_ALL := $(wildcard src/*.c src/util/*.c src/json/*.c src/core/*.c src/cli/*.c \
        src/net/*.c src/mcp/*.c src/tui/*.c \
        src/backends/openai/*.c) src/lib/host_services.c
SRC_ALL := $(C_SRC_ALL) $(CPP_SRC)

# Per-platform source lists (docs/adr/0017). Native transports (sockets, TLS,
# hand-rolled HTTP/1.1) and the poll(2) wrapper are excluded
# from the wasm build wholesale rather than #ifdef-riddled; src/net/net_wasm.c
# replaces the whole seam there (fetch, browser WebSocket, pseudo-fd registry).
SRC_NATIVE := src/net/tcp.c src/net/stream.c src/net/http1.c src/net/http_server.c \
              src/util/tny_poll.c
SRC_WASM_ONLY := src/net/net_wasm.c
SRC_SHARED := $(filter-out $(SRC_NATIVE) $(SRC_WASM_ONLY),$(SRC_ALL))
SRC := $(SRC_SHARED) $(SRC_NATIVE)

TP := third_party/yyjson/yyjson.c third_party/picohttpparser/picohttpparser.c
TP_WASM := third_party/yyjson/yyjson.c

REL_OBJS := $(call objects,$(OBJ_REL),$(SRC)) $(call objects,$(OBJ_REL),$(TP))

# GCC 15's PE LTO fails on shared inline C++ ownership templates, both as
# binds_to_current_def_p ICEs and unresolved LTO-private destructor clones.
# Keep the private C++ release graph consistently native on Windows/GCC;
# C/vendor objects and the final link retain LTO (ADR 0131). All diagnostics,
# optimization and exception flags remain enabled. Other graphs are unchanged.
# LTO_EXEMPT_CPP= re-tests a fixed compiler; use -B to regenerate old objects.
LTO_EXEMPT_CPP ?=
ifeq ($(WINDOWS):$(REL_LTO),1:-flto=auto)
  LTO_EXEMPT_CPP += $(filter %.cpp,$(SRC))
endif
ifneq ($(strip $(LTO_EXEMPT_CPP)),)
$(call objects,$(OBJ_REL),$(LTO_EXEMPT_CPP)): Makefile
$(call objects,$(OBJ_REL),$(LTO_EXEMPT_CPP)): REL_LTO := -fno-lto
endif

# libtny ABI 1: headless runtime only. ACP server/turn are application
# adapters; the ACP client wire remains a library backend.
LIB_APP_EXCLUDE := src/main.c $(filter-out src/cli/globals.c,$(wildcard src/cli/*.c src/tui/*.c))
LIB_SRC := $(SRC_PUBLIC_API) \
           $(filter-out $(LIB_APP_EXCLUDE) $(SRC_PUBLIC_API),$(SRC_SHARED)) \
           $(SRC_NATIVE)
OBJ_PIC := $(BUILD)/pic
LIB_PIC_OBJS := $(call objects,$(OBJ_PIC),$(LIB_SRC)) $(call objects,$(OBJ_PIC),$(TP))
PIC_CFLAGS := $(REL_CFLAGS) -fPIC -fvisibility=hidden \
              -DTNY_SHARED_LIBRARY_BUILD=1 \
              -include src/util/alloc_override.h
OBJ_FAULT_PIC := $(BUILD)/fault-pic
FAULT_PIC_OBJS := $(call objects,$(OBJ_FAULT_PIC),$(LIB_SRC)) \
                  $(call objects,$(OBJ_FAULT_PIC),$(TP))
FAULT_PIC_CFLAGS := $(PIC_CFLAGS) -DTNY_ALLOC_TESTING=1
OBJ_FAULT_SAN_PIC := $(BUILD)/fault-san-pic
FAULT_SAN_PIC_OBJS := $(call objects,$(OBJ_FAULT_SAN_PIC),$(LIB_SRC)) \
                      $(call objects,$(OBJ_FAULT_SAN_PIC),$(TP))
FAULT_SAN_PIC_CFLAGS := $(FAULT_PIC_CFLAGS) -O1 -g \
                        -fsanitize=address,undefined \
                        -fno-omit-frame-pointer
OBJ_TSAN_PIC := $(BUILD)/tsan-pic
TSAN_PIC_OBJS := $(call objects,$(OBJ_TSAN_PIC),$(LIB_SRC)) \
                 $(call objects,$(OBJ_TSAN_PIC),$(TP))
TSAN_PIC_CFLAGS := $(PIC_CFLAGS) -O1 -g -fsanitize=thread \
                   -fno-omit-frame-pointer
FUZZ_CC ?= clang
OBJ_FUZZ := $(BUILD)/fuzz-libfuzzer/obj
FUZZ_OBJS := $(call objects,$(OBJ_FUZZ),$(LIB_SRC)) \
             $(call objects,$(OBJ_FUZZ),$(TP))
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
REL_CXXFLAGS = $(call cxx_flags,$(REL_CFLAGS))
DBG_CXXFLAGS = $(call cxx_flags,$(DBG_CFLAGS))
PIC_CXXFLAGS = $(call cxx_flags,$(PIC_CFLAGS))
FAULT_PIC_CXXFLAGS = $(call cxx_flags,$(FAULT_PIC_CFLAGS))
FAULT_SAN_PIC_CXXFLAGS = $(call cxx_flags,$(FAULT_SAN_PIC_CFLAGS))
TSAN_PIC_CXXFLAGS = $(call cxx_flags,$(TSAN_PIC_CFLAGS))
FUZZ_CXX ?= $(call cxx_driver,$(FUZZ_CC))
FUZZ_CXXFLAGS = $(call cxx_flags,$(FUZZ_CFLAGS))
FUZZ_HARNESS_CXXFLAGS = $(call cxx_flags,$(FUZZ_HARNESS_CFLAGS))

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

TEST_SRC := $(wildcard tests/*.c tests/*.cpp)
TEST_DEPS := $(filter-out src/main.c,$(SRC)) $(SRC_PUBLIC_API) $(TP)
TEST_OBJS := $(call objects,$(OBJ_DBG),$(TEST_DEPS))

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

# Measure shipped artifacts without a fixed product size ceiling.

.PHONY: all release debug test test-unit test-event-schema test-conformance-contract test-extensions-python test-shell-workflows test-install-prefix test-abi test-sdk-python test-sdk-typescript test-sdks test-libtny-fault test-libtny-fault-sanitize test-libtny-tsan test-libtny-mutation test-libtny-fuzz-smoke test-libtny-fuzz size size-check pack smoke bench clean install install-lib install-lib-active lib-shared lib-shared-active lib-shared-compat0 lib-shared-fault lib-shared-fault-sanitize lib-shared-tsan site FORCE

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
	$(CXX) $(REL_CXXFLAGS) $(REL_LTO) $(REL_INLINE) $(REL_SIZE_OPT) -o $@ $^ $(REL_LDFLAGS)
	strip $@ 2>/dev/null || strip -x $@
	@wc -c $@

# A separate, never-installed binary redirects only xAI STT to fake loopback
# fixtures. The shipped adapter is always pinned to https://api.x.ai/v1/stt.
DICTATION_FIXTURE = $(BUILD)/tny-dictation-fixture$(EXE)
DICTATION_FIXTURE_OBJ = $(OBJ_REL)/dictation_xai_fixture.o
$(DICTATION_FIXTURE_OBJ): src/core/dictation_xai.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(REL_CFLAGS) $(REL_LTO) $(REL_INLINE) $(REL_SIZE_OPT) -DTNY_DICTATION_FIXTURE -MMD -MP -c -o $@ $<

$(DICTATION_FIXTURE): $(filter-out $(OBJ_REL)/src/core/dictation_xai.o,$(REL_OBJS)) $(DICTATION_FIXTURE_OBJ)
	$(CXX) $(REL_CXXFLAGS) $(REL_LTO) $(REL_INLINE) $(REL_SIZE_OPT) -o $@ $^ $(REL_LDFLAGS)

.PHONY: dictation-fixture test-dictation wasm-dictation-fixture
dictation-fixture: $(DICTATION_FIXTURE)
test-dictation: $(TEST_BIN) $(BIN) $(DICTATION_FIXTURE)
	./$(TEST_BIN) -s dictation
	TNY=$(abspath $(BIN)) TNY_DICTATION_FIXTURE_BIN=$(abspath $(DICTATION_FIXTURE)) python3 tests/integration/test_dictation.py

$(OBJ_REL)/%.o: %.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(REL_CFLAGS) $(REL_LTO) $(REL_INLINE) $(REL_SIZE_OPT) -MMD -MP $(if $(findstring third_party,$<),-Wno-error -w,) -c -o $@ $<

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

$(OBJ_REL)/%.cpp.o: %.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(CXX) $(REL_CXXFLAGS) $(REL_LTO) $(REL_INLINE) $(REL_SIZE_OPT) -MMD -MP -c -o $@ $<

$(OBJ_DBG)/%.cpp.o: %.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(CXX) $(DBG_CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_PIC)/%.cpp.o: %.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(CXX) $(PIC_CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_FAULT_PIC)/%.cpp.o: %.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(CXX) $(FAULT_PIC_CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_FAULT_SAN_PIC)/%.cpp.o: %.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(CXX) $(FAULT_SAN_PIC_CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_TSAN_PIC)/%.cpp.o: %.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(CXX) $(TSAN_PIC_CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_FUZZ)/%.cpp.o: %.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(FUZZ_CXX) $(FUZZ_CXXFLAGS) -MMD -MP -c -o $@ $<

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
	$(CXX) -o $@ $(LIB_PIC_OBJS) $(LIB_LDFLAGS)

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
	$(CXX) -o $@ $(FAULT_PIC_OBJS) $(LIB_FAULT_LDFLAGS)

$(LIB_FAULT_LINK): $(LIB_FAULT_REAL)
	@mkdir -p $(@D)
	@cd $(@D) && ln -sf $(notdir $(LIB_FAULT_REAL)) $(notdir $@)

$(LIB_FAULT_SAN_REAL): $(FAULT_SAN_PIC_OBJS) $(LIB_EXPORT_FILE)
	@mkdir -p $(@D)
	$(CXX) -o $@ $(FAULT_SAN_PIC_OBJS) $(LIB_FAULT_LDFLAGS) \
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
	$(CXX) -o $@ $(TSAN_PIC_OBJS) $(LIB_LDFLAGS) -fsanitize=thread

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

$(TEST_BIN): $(TEST_OBJS) $(call objects,$(OBJ_DBG),$(TEST_SRC))
	@mkdir -p $(@D)
	$(CXX) $(DBG_CXXFLAGS) -o $@ $^ $(DBG_LDFLAGS)

debug: $(TEST_BIN)

test-unit: $(TEST_BIN) $(BIN)
	./$(TEST_BIN)
	# Regression: running from inside a restricted harness must not hide fixture tools.
	TNY_TOOLS=terminal ./$(TEST_BIN) -s core_suite -t grep_files_fanout_matches_serial_scan
	TNY_TOOLS=terminal+edit ./$(TEST_BIN) -s web_search_suite -t schema_includes_default_search

test-event-schema:
	python3 sdk/schema/check.py

test-conformance-contract:
	python3 sdk/conformance/check.py
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
		-s tests/conformance -p 'test_*.py' -v

test-extensions-python:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/extensions -p 'test_*.py' -v

test-shell-workflows:
	$(BASH) tests/shell/test_workflows.sh
	$(ZSH) tests/shell/test_workflows.sh

.PHONY: test-shell-quick-ask
test-shell-quick-ask:
	ZSH="$(ZSH)" python3 tests/shell/test_quick_ask.py
	ZSH="$(ZSH)" TNY_TEST_TMUX_BIN="$(TMUX_BIN)" python3 tests/shell/test_quick_ask_screen.py

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

FUZZ_SMOKE_OBJ := $(BUILD)/fuzz/fuzz_libtny.o
$(FUZZ_SMOKE_OBJ): tests/fuzz/fuzz_libtny.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(DBG_CFLAGS) -DTNY_FUZZ_STANDALONE=1 -MMD -MP -c -o $@ $<

$(FUZZ_SMOKE_BIN): $(FUZZ_SMOKE_OBJ) $(sort $(TEST_OBJS))
	@mkdir -p $(@D)
	$(CXX) $(DBG_CXXFLAGS) -o $@ $^ $(DBG_LDFLAGS)

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

FUZZ_HARNESS_OBJ := $(BUILD)/fuzz-libfuzzer/fuzz_libtny.o
$(FUZZ_HARNESS_OBJ): tests/fuzz/fuzz_libtny.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(FUZZ_HARNESS_CFLAGS) -MMD -MP -c -o $@ $<

$(FUZZ_BIN): $(FUZZ_HARNESS_OBJ) $(FUZZ_OBJS)
	@mkdir -p $(@D) $(BUILD)/fuzz-artifacts
	$(FUZZ_CXX) $(FUZZ_HARNESS_CXXFLAGS) \
		-o $@ $^ \
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

# Parser stream fuzzing (ADR0114). Portable smoke consumes every checked-in
# seed. The Linux x86_64 campaign shares the fully instrumented production
# object set with ABI fuzzing; FUZZ_RUNS/FUZZ_SECONDS bound each campaign.
PARSER_FUZZ_SRC := tests/fuzz/fuzz_parsers.cpp
PARSER_CORPUS := $(wildcard tests/fuzz/parser-corpus/*)
PARSER_SMOKE_OBJ := $(BUILD)/fuzz/fuzz_parsers.cpp.o
PARSER_SMOKE_BIN := $(BUILD)/fuzz/parser-fuzz-smoke
PARSER_FUZZ_OBJ := $(BUILD)/fuzz-libfuzzer/fuzz_parsers.cpp.o
PARSER_FUZZ_BIN := $(BUILD)/fuzz-libfuzzer/parser-fuzz

$(PARSER_SMOKE_OBJ): $(PARSER_FUZZ_SRC) | $(VERSION_H)
	@mkdir -p $(@D)
	$(CXX) $(DBG_CXXFLAGS) -DTNY_FUZZ_STANDALONE=1 -MMD -MP -c -o $@ $<

$(PARSER_SMOKE_BIN): $(PARSER_SMOKE_OBJ) $(sort $(TEST_OBJS))
	$(CXX) $(DBG_CXXFLAGS) -o $@ $^ $(DBG_LDFLAGS)

$(PARSER_FUZZ_OBJ): $(PARSER_FUZZ_SRC) | $(VERSION_H)
	@mkdir -p $(@D)
	$(FUZZ_CXX) $(FUZZ_HARNESS_CXXFLAGS) -MMD -MP -c -o $@ $<

$(PARSER_FUZZ_BIN): $(PARSER_FUZZ_OBJ) $(FUZZ_OBJS)
	@mkdir -p $(@D) $(BUILD)/parser-fuzz-artifacts
	$(FUZZ_CXX) $(FUZZ_HARNESS_CXXFLAGS) -o $@ $^ \
		$(if $(filter Linux,$(UNAME_S)),-pthread -ldl,)

test-parser-fuzz-smoke: $(PARSER_SMOKE_BIN)
	@test -n "$(PARSER_CORPUS)" || { echo "error: parser corpus is empty" >&2; exit 1; }
	$(PARSER_SMOKE_BIN) $(PARSER_CORPUS)

ifeq ($(UNAME_S)-$(UNAME_M),Linux-x86_64)
test-parser-fuzz: $(PARSER_FUZZ_BIN)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		$(PARSER_FUZZ_BIN) -runs=$(FUZZ_RUNS) -max_total_time=$(FUZZ_SECONDS) \
		-timeout=5 -max_len=131072 -rss_limit_mb=1024 \
		-artifact_prefix=$(BUILD)/parser-fuzz-artifacts/ tests/fuzz/parser-corpus
else
test-parser-fuzz:
	@echo "error: parser libFuzzer gate is supported only on Linux x86_64" >&2
	@exit 2
endif

# Owner fault injection is always enabled. Match sanitizer instrumentation
# to the chosen platform lane: musl/MSYS SANITIZE=0 must not link ASan objects.
OWNER_OBJ_ROOT := $(if $(filter 1,$(SANITIZE)),$(OBJ_FAULT_SAN_PIC),$(OBJ_FAULT_PIC))
OWNER_CFLAGS := $(if $(filter 1,$(SANITIZE)),$(FAULT_SAN_PIC_CFLAGS),$(FAULT_PIC_CFLAGS))
OWNER_CXXFLAGS := $(call cxx_flags,$(OWNER_CFLAGS))
OWNER_LIB_OBJS := $(if $(filter 1,$(SANITIZE)),$(FAULT_SAN_PIC_OBJS),$(FAULT_PIC_OBJS))
# Native request owners: deterministic transport faults plus real yyjson,
# provider headers and allocator. The fixture includes the production owner TU.
NATIVE_REQUEST_TEST_OBJ := $(BUILD)/native-request/native_request_ownership.cpp.o
NATIVE_REQUEST_TEST := $(BUILD)/native-request/ownership-test
NATIVE_REQUEST_SRC := src/util/alloc.c src/util/util.c src/json/json.c \
                      third_party/yyjson/yyjson.c src/core/provider_extras.c src/net/url.c
$(NATIVE_REQUEST_TEST_OBJ): tests/fixtures/native_request_ownership.cpp src/backends/openai/request_owner.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(CXX) $(OWNER_CXXFLAGS) -MMD -MP -c -o $@ $<
$(NATIVE_REQUEST_TEST): $(NATIVE_REQUEST_TEST_OBJ) $(call objects,$(OWNER_OBJ_ROOT),$(NATIVE_REQUEST_SRC))
	$(CXX) $(OWNER_CXXFLAGS) -o $@ $^ $(DBG_LDFLAGS)
test-native-request-ownership: $(NATIVE_REQUEST_TEST)
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(NATIVE_REQUEST_TEST)
.PHONY: test-native-request-ownership
-include $(NATIVE_REQUEST_TEST_OBJ:.o=.d)

# Exhaustive parser allocation failures and semantic mutation oracles use the
# same allocator-instrumented owner objects as the backend ownership suite.
PARSER_OWNER_SRC := src/util/alloc.c src/util/util.c src/json/json.c \
                    third_party/yyjson/yyjson.c src/net/sse.cpp \
                    src/backends/openai/toolcalls.cpp \
                    src/backends/openai/stream_decode.cpp
PARSER_OWNER_OBJS := $(call objects,$(OWNER_OBJ_ROOT),$(PARSER_OWNER_SRC))
PARSER_OWNER_TEST_OBJ := $(BUILD)/parser-ownership/test_ownership.cpp.o
PARSER_OWNER_BIN := $(BUILD)/parser-ownership/ownership-test

$(PARSER_OWNER_TEST_OBJ): tests/test_ownership.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(CXX) $(OWNER_CXXFLAGS) -DTNY_OWNERSHIP_STANDALONE=1 -MMD -MP -c -o $@ $<

$(PARSER_OWNER_BIN): $(PARSER_OWNER_TEST_OBJ) $(PARSER_OWNER_OBJS)
	$(CXX) $(OWNER_CXXFLAGS) -o $@ $^ $(DBG_LDFLAGS)

test-parser-ownership: $(PARSER_OWNER_BIN)
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(PARSER_OWNER_BIN)

test-parser-mutation: test-parser-ownership
	python3 tests/mutation/parser_ownership.py --cxx '$(CXX)' \
		--flags='$(OWNER_CXXFLAGS)' --ldflags='$(DBG_LDFLAGS)' \
		--object-root '$(OWNER_OBJ_ROOT)' --test-object '$(PARSER_OWNER_TEST_OBJ)' \
		--baseline '$(PARSER_OWNER_BIN)' --work-dir '$(BUILD)/parser-mutations' \
		$(PARSER_OWNER_OBJS)

.PHONY: test-parser-ownership test-parser-mutation
-include $(PARSER_OWNER_TEST_OBJ:.o=.d)

SEARCH_OWNER_OBJ := $(BUILD)/parser-ownership/search_ownership.o
SEARCH_OWNER_BIN := $(BUILD)/parser-ownership/search-test
$(SEARCH_OWNER_OBJ): tests/fuzz/search_ownership.c src/core/search_codex.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(OWNER_CFLAGS) -include src/util/alloc_override.h -MMD -MP -c -o $@ $<
$(SEARCH_OWNER_BIN): $(SEARCH_OWNER_OBJ) $(OWNER_LIB_OBJS)
	$(CXX) -o $@ $^ $(DBG_LDFLAGS)
test-search-ownership: $(SEARCH_OWNER_BIN)
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SEARCH_OWNER_BIN)
.PHONY: test-search-ownership
-include $(SEARCH_OWNER_OBJ:.o=.d)

# Launch-plan C facade; only this isolated plan object observes secret wiping.
SUBAGENT_OBJ_ROOT := $(BUILD)/subagent-ownership/obj
SUBAGENT_PLAN_OBJ := $(SUBAGENT_OBJ_ROOT)/src/core/subagent_plan.cpp.o
SUBAGENT_TEST_OBJ := $(OWNER_OBJ_ROOT)/tests/fixtures/subagent_ownership.o
SUBAGENT_TEST_BIN := $(BUILD)/subagent-ownership/ownership-test
SUBAGENT_TEST_OBJS := $(filter-out $(OWNER_OBJ_ROOT)/src/core/subagent_plan.cpp.o,$(OWNER_LIB_OBJS)) $(SUBAGENT_PLAN_OBJ)
SUBAGENT_CXXFLAGS := $(OWNER_CXXFLAGS) -Dsecure_zero=tny_subagent_test_secure_zero
$(SUBAGENT_PLAN_OBJ): src/core/subagent_plan.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(CXX) $(SUBAGENT_CXXFLAGS) -MMD -MP -c -o $@ $<
$(SUBAGENT_TEST_BIN): $(SUBAGENT_TEST_OBJ) $(SUBAGENT_TEST_OBJS)
	@mkdir -p $(@D)
	$(CXX) -o $@ $^ $(DBG_LDFLAGS)
test-subagent-ownership: $(SUBAGENT_TEST_BIN)
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(SUBAGENT_TEST_BIN)
test-subagent-mutation: test-subagent-ownership
	python3 tests/mutation/subagent_ownership.py --cxx '$(CXX)' \
		--flags='$(SUBAGENT_CXXFLAGS)' --ldflags='$(DBG_LDFLAGS)' \
		--object-root '$(SUBAGENT_OBJ_ROOT)' --test-object '$(SUBAGENT_TEST_OBJ)' \
		--baseline '$(SUBAGENT_TEST_BIN)' --work-dir '$(BUILD)/subagent-mutations' \
		$(SUBAGENT_TEST_OBJS)
.PHONY: test-subagent-ownership test-subagent-mutation
-include $(SUBAGENT_TEST_OBJ:.o=.d) $(SUBAGENT_PLAN_OBJ:.o=.d)

# Checkpoint C facade, complete fault-injected object graph (ADR0126).
CHECKPOINT_OWNER_OBJ := $(OWNER_OBJ_ROOT)/tests/fixtures/checkpoint_ownership.o
CHECKPOINT_OWNER_BIN := $(BUILD)/checkpoint-ownership/checkpoint-test
$(CHECKPOINT_OWNER_BIN): $(CHECKPOINT_OWNER_OBJ) $(OWNER_LIB_OBJS)
	@mkdir -p $(@D)
	$(CXX) -o $@ $^ $(DBG_LDFLAGS)
test-checkpoint-ownership: $(CHECKPOINT_OWNER_BIN)
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(CHECKPOINT_OWNER_BIN)
test-checkpoint-mutation: test-checkpoint-ownership
	python3 tests/mutation/checkpoint_ownership.py --cxx '$(CXX)' \
		--flags='$(OWNER_CXXFLAGS)' --ldflags='$(DBG_LDFLAGS)' \
		--object-root '$(OWNER_OBJ_ROOT)' --test-object '$(CHECKPOINT_OWNER_OBJ)' \
		--baseline '$(CHECKPOINT_OWNER_BIN)' --work-dir '$(BUILD)/checkpoint-mutations' \
		$(OWNER_LIB_OBJS)
.PHONY: test-checkpoint-ownership test-checkpoint-mutation
-include $(CHECKPOINT_OWNER_OBJ:.o=.d)

# Every C++ object uses the same test-only owner-counter definitions. The
# remaining C unit objects retain their normal ASan/UBSan instrumentation.
OWNER_INSTRUMENTED_SRC := $(sort src/util/alloc.c $(filter %.cpp,$(TEST_DEPS)) \
    src/core/runtime.c tests/test_runtime.c tests/test_openai.c $(filter %.cpp,$(TEST_SRC)))
OWNER_BACKEND_OBJS := $(filter-out $(call objects,$(OBJ_DBG),$(OWNER_INSTRUMENTED_SRC)),\
    $(TEST_OBJS) $(call objects,$(OBJ_DBG),$(TEST_SRC))) \
    $(call objects,$(OWNER_OBJ_ROOT),$(OWNER_INSTRUMENTED_SRC))
OWNER_BACKEND_BIN := $(BUILD)/parser-ownership/backend-test
$(OWNER_BACKEND_BIN): $(OWNER_BACKEND_OBJS)
	@mkdir -p $(@D)
	$(CXX) -o $@ $^ $(DBG_LDFLAGS)
test-parser-backend-ownership: $(OWNER_BACKEND_BIN)
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(OWNER_BACKEND_BIN) -s openai_suite
.PHONY: test-parser-backend-ownership test-runtime-ownership
RUNTIME_TEST_OBJS := $(OWNER_BACKEND_OBJS)
RUNTIME_TEST := $(OWNER_BACKEND_BIN)
test-runtime-ownership: $(RUNTIME_TEST)
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(RUNTIME_TEST) -s runtime_suite

.PHONY: test-parser-fuzz-smoke test-parser-fuzz test-cpp-build
test-cpp-build:
	python3 tests/integration/test_cpp_build.py

SAN_CUSTOM_HOST := $(BUILD)/fault-san/libtny-custom-tools-sanitizer
$(SAN_CUSTOM_HOST): tests/integration/libtny_custom_tools.c $(LIB_FAULT_SAN_REAL)
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined -o $@ $< $(LIB_FAULT_SAN_REAL) -pthread \
		-Wl,-rpath,$(CURDIR)/$(dir $(LIB_FAULT_SAN_REAL))

SAN_CUSTOM_CPP_HOST := $(BUILD)/fault-san/libtny-custom-tools-cpp-sanitizer
$(SAN_CUSTOM_CPP_HOST): tests/integration/libtny_custom_tools_cpp.cpp $(LIB_FAULT_SAN_REAL)
	@mkdir -p $(@D)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Iinclude -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined -o $@ $< $(LIB_FAULT_SAN_REAL) -pthread \
		-Wl,-rpath,$(CURDIR)/$(dir $(LIB_FAULT_SAN_REAL))

# Provider OOM regressions use the complete allocator-instrumented object graph
# (ADR 0117): real ACP/Cursor/OpenAI backends with injected C and C++ owners.
PROVIDER_FAULT_TEST_SRC :=                            tests/test_openai.c tests/test_ownership.cpp \
                           tests/integration/libtny_provider_fault_host.c
PROVIDER_FAULT_TEST := $(BUILD)/lib-fault/provider-faults
PROVIDER_FAULT_SAN_TEST := $(BUILD)/lib-fault-san/provider-faults
PROVIDER_FAULT_TEST_OBJS := $(call objects,$(OBJ_FAULT_PIC),$(PROVIDER_FAULT_TEST_SRC))
PROVIDER_FAULT_SAN_TEST_OBJS := $(call objects,$(OBJ_FAULT_SAN_PIC),$(PROVIDER_FAULT_TEST_SRC))
$(PROVIDER_FAULT_TEST): $(PROVIDER_FAULT_TEST_OBJS) $(FAULT_PIC_OBJS)
	@mkdir -p $(@D)
	$(CXX) -o $@ $^ $(REL_LDFLAGS)
$(PROVIDER_FAULT_SAN_TEST): $(PROVIDER_FAULT_SAN_TEST_OBJS) $(FAULT_SAN_PIC_OBJS)
	@mkdir -p $(@D)
	$(CXX) -o $@ $^ $(REL_LDFLAGS) -fsanitize=address,undefined
# Native request/pending ownership uses the complete real runtime fault graph.
NATIVE_RUNTIME_TEST := $(if $(filter 1,$(SANITIZE)),$(PROVIDER_FAULT_SAN_TEST),$(PROVIDER_FAULT_TEST))
NATIVE_RUNTIME_OBJS := $(call objects,$(OWNER_OBJ_ROOT),$(PROVIDER_FAULT_TEST_SRC)) $(OWNER_LIB_OBJS)
test-native-lifecycle: $(NATIVE_RUNTIME_TEST)
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(NATIVE_RUNTIME_TEST) -s openai_suite

test-native-mutation: test-native-request-ownership test-native-lifecycle
	python3 tests/mutation/native_ownership.py --cc '$(CC)' --cxx '$(CXX)' \
		--cflags '$(OWNER_CFLAGS) -include src/util/alloc_override.h' --cxxflags '$(OWNER_CXXFLAGS)' \
		--ldflags='$(DBG_LDFLAGS)' --object-root '$(OWNER_OBJ_ROOT)' \
		--request-bin '$(NATIVE_REQUEST_TEST)' --runtime-bin '$(NATIVE_RUNTIME_TEST)' \
		--request-objects '$(call objects,$(OWNER_OBJ_ROOT),$(NATIVE_REQUEST_SRC))' \
		--work-dir '$(BUILD)/native-mutations' $(NATIVE_RUNTIME_OBJS)
ifeq ($(UNAME_S),Darwin)
test-native-leaks: $(NATIVE_REQUEST_TEST) $(NATIVE_RUNTIME_TEST)
	leaks --atExit -- $(NATIVE_REQUEST_TEST)
	leaks --atExit -- $(NATIVE_RUNTIME_TEST) -s openai_suite
else
test-native-leaks: $(NATIVE_REQUEST_TEST) $(NATIVE_RUNTIME_TEST)
	valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite,indirect $(NATIVE_REQUEST_TEST)
	valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite,indirect $(NATIVE_RUNTIME_TEST) -s openai_suite
endif
.PHONY: test-native-lifecycle test-native-mutation test-native-leaks

test-libtny-fault: $(PROVIDER_FAULT_TEST)
test-libtny-fault-sanitize: $(PROVIDER_FAULT_SAN_TEST)
-include $(PROVIDER_FAULT_TEST_OBJS:.o=.d) $(PROVIDER_FAULT_SAN_TEST_OBJS:.o=.d)

# Behavioral runtime/provider mutants (ADR 0116/0117): private copies only.
test-runtime-mutation:
	python3 tests/mutation/runtime_critical.py
.PHONY: test-runtime-mutation

# Runner/job ownership faults (ADR 0118) bind the real runner.cpp/jobs.cpp
# sources into one fixture with real fd/pipe/flock boundaries, an
# allocator-instrumented alloc.c, and syscall-faulting copies of the unchanged
# C host seams. Everything else is the ordinary debug object graph.
RUNNER_OWNERSHIP := $(BUILD)/runner-ownership/runner-ownership
RUNNER_HOST_ALLOC := $(BUILD)/runner-host/alloc.o
RUNNER_OWNERSHIP_OBJS = $(filter-out $(OBJ_DBG)/src/core/runner.cpp.o $(OBJ_DBG)/src/core/jobs.cpp.o \
    $(OBJ_DBG)/src/util/alloc.o $(OBJ_DBG)/src/util/jobs_host.o $(OBJ_DBG)/src/util/process.o,\
    $(sort $(TEST_OBJS))) $(BUILD)/runner-host/jobs_host.o $(BUILD)/runner-host/process.o \
    $(OBJ_DBG)/tests/fixtures/resource_host_faults.o $(RUNNER_HOST_ALLOC)
$(RUNNER_HOST_ALLOC): src/util/alloc.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(DBG_CFLAGS) -DTNY_ALLOC_TESTING=1 -MMD -MP -c -o $@ $<
$(BUILD)/runner-host/jobs_host.o: src/util/jobs_host.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(DBG_CFLAGS) -Dopen=tny_resource_open -Dwrite=tny_resource_write -Dfsync=tny_resource_fsync -Drename=tny_resource_rename -MMD -MP -c -o $@ $<
$(BUILD)/runner-host/process.o: src/util/process.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(CC) $(DBG_CFLAGS) -Dfcntl=tny_resource_fcntl -Dposix_spawn=tny_resource_spawn -MMD -MP -c -o $@ $<
$(BUILD)/runner-ownership/runner_ownership.cpp.o: tests/fixtures/runner_ownership.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(CXX) $(DBG_CXXFLAGS) -DTNY_ALLOC_TESTING=1 -MMD -MP -c -o $@ $<
$(RUNNER_OWNERSHIP): $(BUILD)/runner-ownership/runner_ownership.cpp.o $(RUNNER_OWNERSHIP_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(DBG_CXXFLAGS) -o $@ $^ $(DBG_LDFLAGS)
test-runner-ownership: $(RUNNER_OWNERSHIP)
	@directory=$$(mktemp -d "$${TMPDIR:-/tmp}/tny-ownership.XXXXXX"); \
	  $(RUNNER_OWNERSHIP) "$$directory"; result=$$?; rm -rf "$$directory"; exit $$result
test-runner-mutation:
	python3 tests/mutation/runner_critical.py
.PHONY: test-runner-ownership test-runner-mutation
-include $(BUILD)/runner-ownership/runner_ownership.cpp.d $(BUILD)/runner-host/jobs_host.d \
    $(BUILD)/runner-host/process.d $(RUNNER_HOST_ALLOC:.o=.d) \
    $(OBJ_DBG)/tests/fixtures/resource_host_faults.d

test-libtny-fault-sanitize: lib-shared-fault-sanitize $(SAN_HOST) $(SAN_CUSTOM_HOST) $(SAN_CUSTOM_CPP_HOST)
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
	# Python dlopens the C++ library after ASan initializes. Load its exception
	# runtime up front too, so ASan can resolve __cxa_throw (sanitizers #934).
	@runtime="$$($(CC) -print-file-name=libasan.so)"; \
	cxx_runtime="$$($(CXX) -print-file-name=libstdc++.so)"; \
	test -f "$$runtime" || { echo "error: ASan runtime not found" >&2; exit 1; }; \
	test -f "$$cxx_runtime" || { echo "error: C++ runtime not found" >&2; exit 1; }; \
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	TNY_TEST_ASAN_RUNTIME="$$runtime" TNY_TEST_CXX_RUNTIME="$$cxx_runtime" \
	LD_PRELOAD="$$runtime:$$cxx_runtime" \
	python3 tests/integration/test_libtny_faults.py $(LIB_FAULT_SAN_REAL)
endif
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		python3 tests/integration/libtny_sanitizer_launcher.py $(SAN_HOST)
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	TNY_CUSTOM_TOOL_HOST=$(SAN_CUSTOM_HOST) \
		python3 tests/integration/test_libtny_custom_tools.py
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(UNAME_S)),0,1):halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	TNY_CUSTOM_TOOL_HOST=$(SAN_CUSTOM_CPP_HOST) TNY_CUSTOM_TOOL_COMPLETION_OOM=1 \
		python3 tests/integration/test_libtny_custom_tools.py

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

test: dictation-fixture test-unit test-event-schema test-conformance-contract test-extensions-python test-install-prefix test-help-flags test-shell-quick-ask release
	tests/integration/run.sh

size: release
	@wc -c $(BIN)

# Compatibility target: validate the artifact and report bytes/dependencies.
size-check: release
	@test -f "$(BIN)" && test -s "$(BIN)" && test -x "$(BIN)" || { \
		echo "error: missing, empty or non-executable artifact: $(BIN)" >&2; exit 1; }
	@magic=$$(od -An -tx1 -N4 "$(BIN)" | tr -d ' \n'); \
	case "$$magic" in 7f454c46|4d5a????|feedface|feedfacf|cefaedfe|cffaedfe|cafebabe|bebafeca|cafebabf|bfbafeca) ;; \
	*) echo "error: unrecognized executable artifact: $(BIN)" >&2; exit 1 ;; esac
	@wc -c "$(BIN)"
	@if command -v otool >/dev/null 2>&1; then otool -L "$(BIN)"; \
	elif command -v objdump >/dev/null 2>&1; then \
		objdump -p "$(BIN)" | grep -E 'NEEDED|DLL Name:' || \
			echo "No linked dependencies reported (static artifact or unsupported inspection)."; \
	else echo "Dependency inspection unavailable (otool/objdump not installed)."; fi

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
	cp shell/tny.zsh "$(DESTDIR)$(PREFIX)/share/tny/"
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
ANALYZER_CXX ?= $(call cxx_driver,$(ANALYZER_CC))

# First-party scopes only; third_party/ and the frozen ABI and bench task
# fixtures stay exempt (reformatting a bench fixture changes the task).
# Derive these lists from the tracked tree so new source and shell files enter
# the quality gate automatically instead of depending on maintained globs.
# Tracked *and* untracked-but-not-ignored sources: a file in flight is
# exactly the one whose formatting has not been checked yet.
SOURCE_FILES := $(shell if $(GIT) rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
	$(GIT) ls-files --cached --others --exclude-standard; \
	else find . -type d \( -name .git -o -name build -o -name 'build-*' \
	-o -name .cache -o -name .worktrees -o -name .claude -o -name node_modules \
	-o -name .venv -o -name venv -o -name gen -o -name dist -o -name out \) -prune \
	-o -type f -print | sed 's|^./||'; fi)
# Ignore staged deletions; source archives remain lintable without Git.
FMT_SRC := $(sort $(wildcard $(filter %.c %.h %.cpp %.hpp,\
	$(filter-out third_party/% tests/abi/fixtures/% tests/bench/fixtures/%,$(SOURCE_FILES)))))
SH_SRC  := $(sort $(wildcard $(filter %.sh,$(SOURCE_FILES))))
SHFMT_FLAGS := -i 4 -ci -sr
JS_SRC  := docs/assets/site.js docs/assets/term-core.js docs/assets/term-wasm.js \
           $(wildcard site/assets/*.js src/wasm/*.js tests/site/*.js \
           sdk/typescript/examples/*.mjs sdk/typescript/scripts/*.mjs \
           sdk/typescript/test/*.mjs) \
           $(wildcard sdk/typescript/dist/*.mjs)

# clang-tidy analyzes the native translation units with the release flag
# set (minus -Werror; WarningsAsErrors in .clang-tidy is the gate).
TIDY_SRC    := $(sort $(SRC) $(SRC_PUBLIC_API))
TIDY_C_SRC = $(filter %.c,$(TIDY_SRC))
TIDY_CPP_SRC = $(filter %.cpp,$(TIDY_SRC))
# scripts/tidy_cpp.py discovers the selected CXX driver's STL/system paths;
# a standalone clang-tidy wheel may not know the host toolchain layout.
TIDY_CXXFLAGS = $(call cxx_flags,$(TIDY_CFLAGS))
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

WARN_STRICT_CXX = $(filter-out -Wstrict-prototypes -Wmissing-prototypes,$(WARN_STRICT)) \
                  -Wnon-virtual-dtor -Woverloaded-virtual

format:
	$(CLANG_FORMAT) -i $(FMT_SRC)
	$(RUFF) format .
	$(SHFMT) -w $(SHFMT_FLAGS) $(SH_SRC)

format-c-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FMT_SRC)

format-check: format-c-check
	$(RUFF) format --check .
	$(SHFMT) -d $(SHFMT_FLAGS) $(SH_SRC)

# Share Clang's path-sensitive C++ checks between tidy and analyze. GCC's
# separate C++ lane adds the factory regression and independent defect controls.
.PHONY: analyze-cpp
analyze-cpp: $(VERSION_H)
	$(if $(TIDY_CPP_SRC),python3 scripts/tidy_cpp.py --cxx '$(CXX)' --tidy '$(CLANG_TIDY)' $(TIDY_CPP_SRC) -- $(TIDY_CXXFLAGS),:)

tidy: $(VERSION_H) analyze-cpp
	$(if $(TIDY_C_SRC),$(CLANG_TIDY) --quiet $(TIDY_C_SRC) -- $(TIDY_CFLAGS),:)

warn-strict: $(VERSION_H)
	$(if $(TIDY_C_SRC),$(CC) $(REL_CFLAGS) $(WARN_STRICT) -fsyntax-only $(TIDY_C_SRC),:)
	$(if $(TIDY_CPP_SRC),$(CXX) $(REL_CXXFLAGS) $(WARN_STRICT_CXX) -fsyntax-only $(TIDY_CPP_SRC),:)

# GCC's path-sensitive analyzer (leaks, use-after-free, fd/stream misuse).
# Complementary to clang-tidy; Linux CI runs it, gcc is required.
# double-free is off: it misreads the oom-flag-guarded free in buf_detach
# (src/util/util.c) and flags every caller of path_join.
analyze: $(VERSION_H) analyze-cpp analyze-cpp-gcc
	@for f in $(TIDY_C_SRC); do \
		$(ANALYZER_CC) $(STD) $(WARN) $(INC) $(DEFS) -fanalyzer -O1 \
			-Wno-analyzer-double-free \
			-c -o /dev/null $$f || exit 1; \
	done
	@echo "analyze: $(words $(SRC) $(SRC_PUBLIC_API)) files clean"



.PHONY: analyze-cpp-gcc
analyze-cpp-gcc: $(VERSION_H) test-cpp-analyzer
	@for f in $(TIDY_CPP_SRC); do \
		$(ANALYZER_CXX) $(call cxx_flags,$(STD) $(WARN) $(INC) $(DEFS)) \
			-fanalyzer -O1 -c -o /dev/null $$f || exit 1; \
	done
	@echo "analyze-cpp-gcc: $(words $(TIDY_CPP_SRC)) files clean"

.PHONY: test-cpp-analyzer
test-cpp-analyzer:
	python3 tests/build/test_cpp_analyzer.py --cxx '$(ANALYZER_CXX)' \
		--flags '$(call cxx_flags,$(STD) $(WARN) $(INC) $(DEFS))'

lint-py:
	$(RUFF) check .

lint-sh:
	$(SHELLCHECK) $(SH_SRC)
	$(ZSH) -n shell/tny.zsh

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

quality: format-check tidy warn-strict lint-py lint-sh lint-workflows lint-js $(QUALITY_ANALYZE)
	@if [ -z "$(QUALITY_ANALYZE)" ]; then \
		echo "quality: GCC -fanalyzer skipped on $(UNAME_S); CI runs it on Linux"; \
	fi

.PHONY: format format-c-check format-check tidy warn-strict analyze lint-py lint-sh lint-workflows lint-js quality

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
# terminal_task_suite also forks a detached waiter; the inherited atExit hook
# stops that waiter before its launch handshake (ADR 0136).
LEAK_SUITE_SKIP := mcp_suite runner_suite \
	session_bg_suite ssh_suite terminal_task_suite task_workspace_process_suite
LEAK_SUITES = $(filter-out $(LEAK_SUITE_SKIP),\
	$(if $(wildcard tests/test_main.c),$(shell sed -n 's/.*RUN_SUITE(\([A-Za-z0-9_]*\)).*/\1/p' tests/test_main.c)))

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
EMCXX       ?= $(patsubst %emcc,%em++,$(EMCC))
OBJ_WASM     = $(BUILD)/wasm/obj
WASM_NODE    = $(BUILD)/wasm/tny.js
WASM_WEB     = $(BUILD)/wasm/tny-web.mjs
WASM_SRC    := $(SRC_SHARED) $(SRC_WASM_ONLY) $(TP_WASM)
WASM_OBJS   := $(call objects,$(OBJ_WASM),$(WASM_SRC))
WASM_CFLAGS  = $(STD) $(WARN) $(INC) $(DEFS) -Os
WASM_CXXFLAGS = $(call cxx_flags,$(WASM_CFLAGS))
# -fexceptions at compile AND link enables portable JS exception catching:
# https://emscripten.org/docs/porting/exceptions.html
# Asyncify is the suspension mechanism (JSPI is Chrome-only, COOP/COEP for
# workers cannot be set on GitHub Pages). Broad instrumentation first; narrow
# later if measurements justify it (docs/adr/0017 footguns).
WASM_LDFLAGS = -fexceptions -Os -sASYNCIFY -sASYNCIFY_STACK_SIZE=131072 \
               -sALLOW_MEMORY_GROWTH -sEXIT_RUNTIME=1 -sSTACK_SIZE=1048576

$(OBJ_WASM)/%.o: %.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(EMCC) $(WASM_CFLAGS) -MMD -MP $(if $(findstring third_party,$<),-Wno-error -w,) -c -o $@ $<

$(OBJ_WASM)/%.cpp.o: %.cpp | $(VERSION_H)
	@mkdir -p $(@D)
	$(EMCXX) $(WASM_CXXFLAGS) -MMD -MP -c -o $@ $<

$(WASM_NODE): $(WASM_OBJS) src/wasm/pre_node.js
	@mkdir -p $(@D)
	$(EMCXX) $(WASM_LDFLAGS) -sENVIRONMENT=node -sNODERAWFS \
		--pre-js src/wasm/pre_node.js -o $@ $(WASM_OBJS)
	@printf '#!/bin/sh\nexec node "%s" "$$@"\n' "$$(cd $(@D) && pwd)/tny.js" > $(@D)/tny
	@chmod +x $(@D)/tny
	@wc -c $@ $(@:.js=.wasm)

$(WASM_WEB): $(WASM_OBJS) src/wasm/pre_web.js
	@mkdir -p $(@D)
	$(EMCXX) $(WASM_LDFLAGS) -sENVIRONMENT=web -sMODULARIZE -sEXPORT_ES6 \
		-sINVOKE_RUN=0 -sEXPORTED_RUNTIME_METHODS=callMain,FS,ENV \
		--pre-js src/wasm/pre_web.js -o $@ $(WASM_OBJS)
	@wc -c $@ $(@:.mjs=.wasm)

WASM_DICTATION_FIXTURE_OBJ = $(OBJ_WASM)/dictation_xai_fixture.o
WASM_DICTATION_FIXTURE = $(BUILD)/wasm/tny-dictation-fixture.js
$(WASM_DICTATION_FIXTURE_OBJ): src/core/dictation_xai.c | $(VERSION_H)
	@mkdir -p $(@D)
	$(EMCC) $(WASM_CFLAGS) -DTNY_DICTATION_FIXTURE -MMD -MP -c -o $@ $<

$(WASM_DICTATION_FIXTURE): $(filter-out $(OBJ_WASM)/src/core/dictation_xai.o,$(WASM_OBJS)) $(WASM_DICTATION_FIXTURE_OBJ) src/wasm/pre_node.js
	$(EMCXX) $(WASM_LDFLAGS) -sENVIRONMENT=node -sNODERAWFS \
		--pre-js src/wasm/pre_node.js -o $@ $(filter %.o,$^)
	@printf '#!/bin/sh\nexec node "%s" "$$@"\n' "$(abspath $(WASM_DICTATION_FIXTURE))" > $(@D)/tny-dictation-fixture
	@chmod +x $(@D)/tny-dictation-fixture

wasm-dictation-fixture: $(WASM_DICTATION_FIXTURE)

wasm: $(WASM_NODE)
wasm-web: $(WASM_WEB)

# Report glue and wasm bytes without a size ceiling; missing inputs still fail.
wasm-size-check: wasm
	@set -e; \
	for artifact in "$(WASM_NODE)" "$(WASM_NODE:.js=.wasm)"; do \
		test -f "$$artifact" && test -s "$$artifact" || { \
			echo "error: missing or empty wasm artifact: $$artifact" >&2; exit 1; }; \
	done; \
	magic=$$(od -An -tx1 -N8 "$(WASM_NODE:.js=.wasm)" | tr -d ' \n'); \
	test "$$magic" = 0061736d01000000 || { echo "error: invalid wasm header" >&2; exit 1; }; \
	js_bytes=$$(wc -c < "$(WASM_NODE)"); \
	wasm_bytes=$$(wc -c < "$(WASM_NODE:.js=.wasm)"); \
	echo "$$js_bytes $(WASM_NODE)"; \
	echo "$$wasm_bytes $(WASM_NODE:.js=.wasm)"; \
	echo "$$((js_bytes + wasm_bytes)) wasm artifact (JavaScript glue + wasm)"; \
	echo "Runtime dependencies: Node.js for this target; browser host for wasm-web."

.PHONY: wasm wasm-web wasm-size-check

# Default cleanup includes disposable root-level build variants. A custom
# BUILD keeps cleanup scoped, so independent build lanes can coexist.
clean:
	rm -rf -- "$(BUILD)" dist
ifeq ($(BUILD),build)
	@for dir in ./build-*; do \
		if [ -d "$$dir" ] && [ ! -L "$$dir" ]; then \
			rm -rf -- "$$dir" || exit $$?; \
		fi; \
	done
endif

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
         $(patsubst %.o,%.d,$(call objects,$(OBJ_DBG),$(TEST_SRC)))

-include $(DICTATION_FIXTURE_OBJ:.o=.d) $(WASM_DICTATION_FIXTURE_OBJ:.o=.d)

-include $(WASM_OBJS:.o=.d) $(FUZZ_SMOKE_OBJ:.o=.d) $(FUZZ_HARNESS_OBJ:.o=.d)

-include $(PARSER_SMOKE_OBJ:.o=.d) $(PARSER_FUZZ_OBJ:.o=.d)

.PHONY: test-size-policy
test-size-policy:
	python3 tests/packaging/test_size_budget.py

test test-unit: test-size-policy
