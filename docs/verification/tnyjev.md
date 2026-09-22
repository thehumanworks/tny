# tnyjev verification — 2026-09-22

Scope: [tnyjev](../tnyjev.md), the isolated typed C11 Jev module and the
`score`/`choose` CLI adapters. No live TypeSafe inference or real credentials
were used. Fixtures use loopback HTTP and synthetic credentials.

## Input identity

Implementation is in the same commit as this record, based on `c2cd74a`.
Critical source SHA-256 values after the final HTTP-header cleanup:

| File | SHA-256 |
| --- | --- |
| `src/core/tnyjev.c` | `5db722e32b6c1827132e15b59d1463d0970fa9cc9b84d238df1704f47820d59e` |
| `src/core/tnyjev.h` | `9d3d39958366ee135b899ba46c1e8263015ade449ea139a0031e53f71fefe5c8` |
| `src/cli/cmd_jev.c` | `1f841ca4ee4a7dd419d7aa52b83dce358a7b58b35d29bf96c199cab12c964ae2` |
| `tests/test_tnyjev.c` | `dbe21698090391f0b3102c33eac2a10b71562a2c41709ea1b620bb28d33ee430` |
| `tests/integration/test_tnyjev.py` | `860d83f70066a5cd749322dbe53e68619a8fef98c70a19a2592a0f0cc77d1ed3` |

## Observed checks

Host: Darwin arm64, pinned mise tools, Emscripten 6.0.8 for actual wasm builds.

| Check | Result |
| --- | --- |
| `mise install` | Exit 0; pinned tools available |
| `make -j6 test` | Exit 0; unit/protocol/integration gate passed. Unit runner: 545 passed, 1 skipped; the new native Jev integration suite passed all 13 tests |
| `make quality` | Exit 0 after fixing the new module's explicit `strncmp != 0` comparison. GCC analyzer explicitly skipped on Darwin, as designed |
| `make -j6 leaks` | Exit 0, clean macOS leak gate; includes `tnyjev_suite` |
| `UBSAN_OPTIONS=halt_on_error=1 build/tny-test -s tnyjev_suite` | 10 passed, 242 assertions; ASan/UBSan debug build |
| `python3 tests/integration/test_tnyjev.py` | 13 passed, including duplicate-header regression, errors, bounds, timeout, cancellation, chunked and every-body-split cases |
| `make wasm wasm-web` | Both actual artifacts built successfully with Emscripten 6.0.8 |
| `TNY=build/wasm/tny python3 tests/integration/test_tnyjev.py` | 12 passed; native POSIX signal case explicitly skipped |
| `tests/integration/test_site_wasm.py` with Playwright/Chromium | Existing landing-terminal banner, mock chat/tool turn, clean quit and named-provider turn passed on the rebuilt web artifact |
| Standalone C++20 translation unit including `core/tnyjev.h` | Passed `-Wall -Wextra -Werror -fsyntax-only`; no runtime/context headers needed |
| `tests/mutation/mutate.py --focus tnyjev --fast --test tnyjev` | Exit 0; all 6 valid mutants killed by unit tests, 2 uncompilable variants excluded, no survivors or timeouts; restored build and baseline passed |
| Mutation inventory regression | 7 passed; executable invalidation forces a real relink instead of accepting stale mutation binaries |
| `git diff --check` | Clean |

The broad suite began before two local HTTP-only cleanups (an explicit string
comparison and removal of a duplicate User-Agent). Its later Jev integration
stage ran against the updated release binary; the dedicated unit, native HTTP,
wasm, quality and leak checks were also repeated on the final source above.
Unchanged non-Jev paths did not require another full integration traversal.

The shared HTTP transport already supplies User-Agent. tnyjev now supplies
only Authorization, Content-Type and Accept. The fixture asserts that no
request has duplicate User-Agent fields. Malformed-response unit tests also
exercise the explicit object guard before yyjson object iteration.

## Footprint

`make size-check` verified the stripped native Release artifact at **1,269,840
bytes** with the pre-commit version stamp, linked to system `libc++` and
`libSystem`. The Node and browser wasm
payloads are each **1,571,553 bytes** (JS bootstrap sizes excluded). These are
absolute local artifact measurements, not before/after speed or size claims.
No vendor SDK, new runtime dependency, or external model executable was added.

## Boundaries

These are local implementation checks, not independent acceptance or live
model-quality evidence. Independent peer review was unavailable in this
session (the team CLI required a trusted terminal adapter).

Live Jev authentication, entitlement, model quality, calibration, latency and
browser CORS at the real endpoint remain unverified. The browser smoke above
checks the existing landing terminal, not a new Jev browser login UI. Linux
GCC analysis and Nix hermetic checks were not run locally. CI includes the
new Jev fixture in its real Node-wasm lane.
