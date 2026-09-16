# ADR 0122: MSYS GCC LTO exemption for the jobs module

Date: 2026-09-16. Status: accepted for the C++ ownership branch (ADR 0114).
ADR 0121 is reserved for the size and priority revision.

## Context

CI run 35137976075 built every object on the `windows-x86_64` lane (MSYS2
`MSYS`, GCC 15.3.0 for `x86_64-pc-cygwin`) and failed only at the release
link:

```
during GIMPLE pass: alias
src/core/jobs.cpp: In function 'jobs_launch_worker.constprop':
src/core/jobs.cpp:1719:12: internal compiler error: in binds_to_current_def_p, at symtab.cc:2598
make[1]: *** [/tmp/cchl7WE4.mk:28: /tmp/ccmj0Rfu.ltrans13.ltrans.o] Error 1
lto-wrapper: fatal error: make returned 2 exit status
```

The same sources, flags and `-flto=auto` link passed on glibc x86_64, musl,
aarch64, macOS, TSan, fuzz and SDK lanes, and release v0.11.1 (2026-09-15)
shipped `tny-windows-x86_64.exe` from the same lane with the C launcher
(`jobs.c`, before ADR 0118).

Reading GCC 15's sources (`releases/gcc-15`) gives the mechanism:

- `symtab.cc:2598` is `gcc_assert (externally_visible)` inside
  `symtab_node::binds_to_current_def_p`, reached for a defined, non-external
  symbol that `decl_binds_to_current_def_p` says does not bind locally.
- The alias pass reaches it from `determine_global_memory_access`
  (`tree-ssa-structalias.cc`): `node = cgraph_node::get (gimple_call_fndecl
  (stmt))` and `node->binds_to_current_def_p ()`. The questioned symbol is a
  **callee function** of the launcher clone, never a variable such as
  `environ`. `extern "C" char **environ;` is the same declaration the C
  modules use and is not involved.
- `decl_binds_to_current_def_p` starts with `targetm.binds_local_p`. On PE
  that is `i386_pe_binds_local_p` (`config/mingw/winnt.cc`), which under
  `#ifndef MAKE_DECL_ONE_ONLY` returns false for any public, defined,
  inline-declared one-only function (PR target/66655). `config/i386/cygming.h`
  never defines `MAKE_DECL_ONE_ONLY`; `config/elfos.h` does. ELF lanes
  therefore bind such symbols locally and never reach the assertion.
- `jobs_launch_worker` is the only launcher that calls the private
  `tny::pipe_pair` / `tny::basic_descriptor` inline members (ADR 0118), which
  are public inline one-only definitions in LTO bytecode; IPA-CP specializes
  the launcher into `.constprop`, and LTRANS compiles that clone.

MSYS2 tracked the same signature (`-flto -Os`, template code, MinGW GCC
10–12) in msys2/MINGW-packages#11726; the reporter observed that `-O2` or
removing calls made it disappear. This is a compiler defect on PE targets,
not a defect in the C++20 module or in a C-facing seam.

## Decision

1. On the MSYS/Cygwin lane with GCC-style LTO (`WINDOWS=1` and
   `REL_LTO = -flto=auto`), `src/core/jobs.cpp` compiles to a native object
   with `-fno-lto`. The Makefile lists it in `LTO_EXEMPT_CPP`; the object's
   recipe keeps `-Os`, `-ffunction-sections`, `-std=c++20`, `-fexceptions`,
   `-fno-rtti`, `-Wall -Wextra -Werror` and the glibc floor include.
2. Every other object (C, vendored, and the other private C++ modules) and
   the executable link keep `-flto=auto`. Clang on MSYS, Linux, macOS, wasm,
   PIC, debug and fault lanes are unchanged. No diagnostic is suppressed and
   no test expectation changes.
3. `LTO_EXEMPT_CPP` is an ordinary make variable: `make release
   LTO_EXEMPT_CPP=` re-tests a fixed toolchain, and a further module can be
   listed with one token if a later MSYS GCC trips the same assertion
   elsewhere (the runner's restart path uses the same descriptor owners but
   did not fail in the recorded run).
4. `tests/integration/test_windows_lto_flags.py` proves the recipe contract
   for MSYS GCC, the override, MSYS Clang and Linux GCC.
   `tests/integration/test_cpp_build.py` builds a real fixture where one C++
   module is exempt, links it into an LTO executable with the host compiler
   and runs its exception path.

## Consequences

- The Windows executable loses cross-module inlining into and out of
  `jobs.cpp` only; the durable-jobs supervisor is not a startup or streaming
  path. The Windows size gate stays at 2.0 MiB; the size effect is measured
  on the hosted lane, not locally.
- A PE-only compiler rule is now recorded next to the object it affects,
  instead of a global `-fno-lto`, `-O2`, `-fno-ipa-cp-clone` or warning
  change.
- Hosted Windows verification remains the CI lane. Local macOS/Linux runs
  prove the recipe and mixed native/LTO link contracts only.

## Evidence

- CI run 35137976075, `windows-x86_64` job, the `c++ ... -o build/tny.exe`
  link step.
- GCC 15 `gcc/symtab.cc` (`binds_to_current_def_p`, line 2598),
  `gcc/tree-ssa-structalias.cc` (`determine_global_memory_access`),
  `gcc/config/mingw/winnt.cc` (`i386_pe_binds_local_p`),
  `gcc/config/i386/cygming.h` and `gcc/config/elfos.h`
  (`MAKE_DECL_ONE_ONLY`).
- https://github.com/msys2/MINGW-packages/issues/11726


When re-testing a fixed toolchain, force regeneration with
`make -B release LTO_EXEMPT_CPP=`; stale objects do not establish that LTO
has been re-enabled. Exempt objects depend on the Makefile so introducing or
editing this workaround rebuilds them in an existing checkout. Current
artifact size authority is ADR0121 (strictly below decimal 6 MB).
