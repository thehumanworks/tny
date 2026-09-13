# ADR 0111: Linux frame-pointer omission, a dead yyjson feature, and the aarch64 64 KiB size cliff

Date: 2026-09-13. Status: accepted. Amends the "retain hardening" line of
[ADR 0103](0103-nix-link-time-runtime-path.md) for one flag of the Nix
package; the budget, the 64 KiB LOAD alignment and the unwind tables are
unchanged.

## Context

The durable-agents and search merges of 2026-09-12/13 (ADRs 0107–0109)
grew the native binary by 45–65 KiB. x86_64 stayed inside the 1 MiB Linux
budget (`docs/size-and-speed.md`); aarch64 did not, on both toolchains:

| aarch64-linux build | before (2026-09-12) | after | budget |
| --- | --- | --- | --- |
| Ubuntu 24.04 GCC 13 (`ci`, release assets) | 986,144 | 1,051,728 | 1,048,576 |
| nixpkgs GCC 15 (`nix flake check`) | 986,088 | 1,051,672 | 1,048,576 |

Neither jump is code: the read-only `R E` segment grew by a few KiB, the
file by 64 KiB. Both linkers lay the executable out as two `LOAD`
segments aligned to 64 KiB (the aarch64 page-size maximum), and with
`-z relro` the end of the RELRO region has to land on a 64 KiB boundary.
The read-write segment's file offset must be congruent to its virtual
address modulo 64 KiB, so the padding between the two segments is

    pad = (64 KiB − relro_size − (rx_end mod 64 KiB)) mod 64 KiB

With `relro_size` ≈ 7.7 KiB the cliff sits where `rx_end mod 64 KiB`
passes ≈ 57.8 KiB: one more byte of code costs 64 KiB of file. Before the
merges the read-only segment ended just below that line (`0xee1e0` on
Ubuntu); after it ended 1,108 bytes (Ubuntu) and 14,332 bytes (nix) past
it. `-z common-page-size=4096` does not move the RELRO alignment on
aarch64 binutils, and `-z norelro` or a smaller `max-page-size` would
weaken the guarantees ADR 0103 keeps. `-fmerge-all-constants` would have
been enough on Ubuntu but is a non-conforming optimisation and was rejected.

Measured on the frozen tree in an aarch64 Ubuntu 24.04 container
(`make release CC=gcc`, stripped bytes / read-only segment bytes):

| Change | file | `R E` segment |
| --- | --- | --- |
| baseline | 1,051,728 | 984,148 |
| `-fomit-frame-pointer -momit-leaf-frame-pointer` | 986,192 | 975,156 |
| + `-DYYJSON_DISABLE_NON_STANDARD` | 986,192 | 973,412 |
| `-Oz`, `-fno-plt`, `-Wl,-O1`, `-fno-jump-tables`, `-fipa-pta`, `-flto-partition=one`, `-mno-outline-atomics` | no gain or larger | |

GCC keeps the frame pointer on aarch64 at every optimisation level (and
Ubuntu's GCC keeps it everywhere by distro policy); that is ~9 KiB of
prologue and epilogue. nixpkgs' GCC already omits it, so on nix the flag
changes nothing; there the extra code is nixpkgs' default hardening set,
`-fzero-call-used-regs=used-gpr` in particular (about 15 KiB of register
clearing on this binary). The Ubuntu-built release assets never carried
that flag.

## Decision

1. **Linux native executables omit the frame pointer**
   (`-fomit-frame-pointer -momit-leaf-frame-pointer`, `REL_SIZE_OPT` in the
   Makefile next to ADR 0102's Clang `-Oz`). Backtraces come from
   `.eh_frame`, which stays. Darwin arm64 requires the frame pointer by
   ABI and is untouched; PIC/library, debug and wasm lanes are untouched.
2. **`YYJSON_DISABLE_NON_STANDARD`** is defined for every build. tny never
   passes a `YYJSON_READ_ALLOW_*` / `YYJSON_WRITE_ALLOW_*` flag, so the
   vendored reader's and writer's comment, NaN/Infinity, trailing-comma and
   invalid-unicode paths are dead code; upstream documents the knob and no
   vendor source changes.
3. **The Nix package disables `zerocallusedregs`** (`hardeningDisable`),
   which brings its hardening set to parity with the published Ubuntu
   builds (PIE, RELRO, bindnow, stack protector, stack clash protection,
   fortify and format checks all stay). This is the one line of ADR 0103's
   "retain hardening" that changes. Result on aarch64-linux (nixpkgs GCC
   15, same container measurement): 986,136 bytes, read-only segment
   958,116 bytes, about 16.7 KiB below the cliff.
4. **The cliff is documented** in `docs/size-and-speed.md`: on aarch64 the
   1 MiB budget is really "read-only segment below ≈ 975 KiB". The
   remaining margin is thin (about 1.8 KiB on Ubuntu after this change);
   the next feature that lands code on that platform must pay for it with
   real code-size work, not another flag.

## Consequences

- aarch64 `ci`, `release` and `nix` lanes are green again; x86_64 shrinks
  by a few KiB as a side effect.
- Frame-pointer-based profilers (`perf --call-graph fp`) need DWARF
  unwinding on Linux builds; `--call-graph dwarf` works.
- The Nix package no longer clears used registers on function return. A
  user who wants nixpkgs' full default set can build with
  `hardeningDisable = [ ]` and accept the size gate failing until the code
  shrinks.
- The margin on aarch64 is recorded so that the next size regression is
  read as a cliff, not as a mystery.
