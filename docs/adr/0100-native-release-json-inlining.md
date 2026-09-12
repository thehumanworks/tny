# 0100 — Compiler-selected JSON helper inlining in native releases

Date: 2026-09-12
Requirements: I-G1, I-G4, unchanged executable budgets; contract A22.

## Decision

Set REL_INLINE to -Dyyjson_inline=inline for native executable release objects
and executable linking, including the separate dictation fixture. Retain
ADR0092's -flto and -Os. Use the pinned yyjson0.12.0 header's existing override
guard; do not alter vendor source. Both public-header and implementation helpers
remain static inline, so this changes the optimizer's inlining choice without
introducing externally linked inline definitions or changing layouts or ABI.

Keep the override out of DEFS/REL_CFLAGS and therefore out of PIC/shared SDK,
debug/sanitizer, strict-analysis and wasm recipes. Retain unwind tables and all
requested features. The four native recipe sites share the dedicated variable.

## Alternatives and consequences

On the same frozen1131-input Linux GCC13.3 aarch64 tree, forced inlining produces
1117160B and compiler-selected inlining986088B. This is current evidence, not a
claim about the final integrated artifact. D069 -Oz alone remained1117160B; D071
-Oz plus unwind omission was1051624B and loses diagnostic capability. General
inlining suppression increased size to1248200B. Switching to Clang -Oz alone
produced1052368B. None of those alternatives met this gate.

The selected change removes the vendor's always_inline attribute through its
existing supported macro guard and leaves optimization to -Os/LTO. Call overhead
and compile decisions can affect performance; no latency improvement is claimed.
No runtime dependency, budget waiver, ABI change or unwind removal is introduced.

## Validation

Independent reviewer /root/jobs_process_review inspected pinned helper linkage,
constant-string runtime fallbacks, callback helpers and flag scope before this
decision. Record effective native/fixture versus PIC/debug/wasm flags; use actual
release image/JSON/provider fixtures and paired tests/bench/bench_ttft.py results.
Final acceptance requires current GCC/Clang/MSYS/musl release behavior, unchanged
ABI/sanitizer/wasm gates, and the final integrated stripped executable below its
original target budget. D074 is a sizing probe, not final completion.

References: [pinned yyjson header](../../third_party/yyjson/yyjson.h),
[GCC inline optimization](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html).
