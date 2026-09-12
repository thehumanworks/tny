# 0092 — Link-time optimization for native CLI release objects only

Date: 2026-09-12
Status: proposed; final integrated verification pending
Requirements: I-G1, I-G4 and the unchanged executable size gates; canonical verification contract A10.

## Decision

Apply generic -flto to the native CLI's release object compilation and executable link, including its separate dictation-fixture compilation/link. Keep a dedicated REL_LTO variable used only at these four recipe sites. Do not change REL_CFLAGS, because PIC/shared SDK, fault/sanitizer and strict-warning paths derive from it. Existing debug/wasm/PIC/static-analysis behavior and public ABI are unchanged. No compiler-dependent fallback silently passes a failed supported-platform build.

## Alternatives and consequences

Raising the1MiB Linux budget would weaken a product invariant. Applying LTO to REL_CFLAGS would expand scope into libraries/sanitizers and require avoidable compatibility work. CLI-only LTO uses existing object separation, introduces no runtime dependency, and recovers size through cross-translation-unit optimization while retaining every requested behavior. A prior disposable GCC -flto=auto result justified measuring this choice but does not verify the production generic spelling. Compile/link time can increase. No unmeasured latency/throughput improvement is claimed.

## Validation

Independent design reviewer e18bd550-33d9-40a2-bd3d-a04de10b2c2c identified the otherwise-missed dictation-fixture link; all four sites are included before implementation. Final acceptance requires clean native Clang/GCC/MSYS2 and musl builds as applicable, real release-binary behavior, strict Nix size check, unchanged ABI inventories and unchanged debug/wasm/compiler-warning gates. Compare baseline/default/final size with source/compiler/flag manifests. Missing platform evidence remains blocked, not exempt.
