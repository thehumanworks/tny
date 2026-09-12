# 0102 — Size optimization for Linux Clang native releases

Date: 2026-09-12
Requirements: unchanged Linux dynamic1MiB budget; I-G1/I-G4; contract A24.
Supersedes only ADR0100's native Linux Clang optimization level.

## Decision

Detect Clang from the actual complete CC command's --version once during Make
parsing. On Linux with that compiler, append REL_SIZE_OPT=-Oz at the four native
CLI/dictation compile/link sites after common -Os. Preserve -flto and the pinned
yyjson inline override. Carry the same flag into native fixture capture code.

Keep GCC, Darwin, MSYS, PIC/shared SDK, debug/sanitizer, strict analysis and wasm
unchanged. Do not modify vendor source, select another compiler or relax budget.
An explicitly overridden compiler command remains authoritative; detection does
not infer the compiler solely from its executable name or host platform.

## Evidence and alternatives

On final frozen1141 inputs, Clang18.1.3 -Os builds1117912B and fails. A separate
-Oz probe builds986840B with unwind metadata retained. GCC13.3 builds986144B
under the existing policy. A global -Oz change would alter unrelated passing
lanes; removing unwind tables would reduce diagnostic capability. This scoped
optimizer setting resolves the measured compiler-specific excess. -Oz can trade
execution speed for code size, so no performance improvement is claimed.

## Validation

Independent /root/jobs_process_review challenged scope, ordering, wrapper
detection and fixture fidelity before implementation. Check expanded actual
recipes and effective compiler/platform flags, unchanged foreign-lane flags,
final Clang release fixtures and paired bench_ttft measurements. Recheck the
final stripped size and preserve the original failure; other required native,
SDK, WASM and Nix gates remain active.

[GCC optimization options](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html)
and the actual installed Clang probe define the measured option behavior.
