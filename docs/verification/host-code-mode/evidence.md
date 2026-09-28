# Host-authorized Python code mode: evidence

## Delivered behavior

The user requested direct host authority instead of the code-mode confinement
introduced in ADR 0179. Work was performed directly in `~/Projects/tny` on `main`.
The production decision is [ADR 0180](../../adr/0180-host-authorized-python-code-cells.md).

Code mode now uses the OS user's filesystem, network and process authority.
`import`, `open`, `eval`, `exec`, `subprocess`, sockets, HTTP and HTTPS are usable.
The child receives the normal environment, minus reserved Tny process-scope
carriers, and starts in the selected local workspace directory. The old
seccomp/macOS pure-computation profile, empty environment, removed builtins and
cell-specific NOFILE/NPROC/FSIZE/CPU/core restrictions are gone. Direct Python
operations are not mediated by nested-tool permission modes, hooks or tool
profiles. Nested `tools.call` operations keep their existing checks.

The bundled standard library includes normal JSON, SSL/hashlib, SQLite, ctypes,
zlib/bzip2/xz/zstd compression, archive tools, asyncio, threading and fork-based
multiprocessing. Native dependencies are pinned and statically linked; runtime
code cells do not require a separately installed Python. GUI/interactive optional
modules, dbm.gnu/dbm.ndbm, ensurepip and venv are not bundled. Third-party packages
are not preinstalled. There is no standalone Python CLI at `sys.executable`;
use installed host programs explicitly, and use the supported `fork` context
rather than assuming multiprocessing can relaunch this embedded interpreter.

This is intentional host access, not a claim of confinement. The host's own OS
permissions, service authentication, network policy and resource limits still
apply. Direct Python runs on the local execution host even under `--ssh`; nested
Tny tools preserve their existing remote routing.

## Execution behavior retained

A fresh child isolates interpreter lifetime, not host authority. The owner can
cancel a cell and stop its ordinary owned descendants. Deadline defaults to five
seconds and can be set up to 600,000 ms for builds and network work. The Python
allocation meter is 1 GiB, not a whole-process/subprocess RSS guarantee. Source
and nested arguments retain their 256 KiB bounds and nested calls their 64-call
budget. Returned stdout/stderr is bounded to 64 KiB by keeping the beginning and
end; noisy commands continue executing instead of being killed for output volume.
No possibly executed operation is automatically replayed after an error.

The framed callback descriptor is not inherited by exec children and is detached
in bare-fork children. Those children can still perform ordinary host work; they
cannot accidentally impersonate the original cell's typed-tool callback stream.
Terminal delivery still requires the final frame, EOF and successful child exit.

## Checkpoints and correction evidence

`045e4ec` contains the recovered Opus/high implementation. `68861ae` fixes two
reproduced failures where malformed subprocess bytes or a truncated Unicode
exception corrupted the outer result protocol. It also corrects stale model
instructions about bundled modules and repairs the isolated build-fixture
closure for the new dependency pins. `a1676de` fixes double-close on output-pipe
setup failure, preserves the original errno, and avoids a dangling stack reference
on atfork registration failure. No existing diagnostic was suppressed.

The descriptor fault test compiles the actual `output_pipe` definition. It checks
successful CLOEXEC/nonblocking setup, all four fcntl failure points and pipe
failure, then rejects three deliberately broken cleanup variants. This is
executable boundary evidence, separate from the pure Lean gate proofs.

## Completed local observations

| Check | Actual result |
| --- | --- |
| Final native unit executable | 619 tests passed, zero failures/skips, 47,502 assertions |
| Focused production code runtime | 22/22 passed, 160 assertions |
| Production execution, Responses and Chat | 21 passed; one explicit wasm-only skip |
| Direct host and verified-HTTPS acceptance | 2/2 passed; real files, environment/cwd, workspace import, subprocess, thread, HTTP and certificate-verified HTTPS |
| Existing execution permissions | 7/7 passed; these cover nested-tool policy, not confinement of direct Python |
| Execution command lifecycle | 2/2 passed |
| Native/unsupported seams and output-pipe fault matrix | 3/3 passed, including negative controls |
| Isolated build-graph fixtures | 22 passed; one explicit missing-Emscripten platform skip |
| Installation and license layout | 1/1 passed |
| Affected job-method rerun on frozen executable | 27/27 passed, including all formerly failing subcases |
| Collective-swarm rerun | 19/19 passed after an earlier teardown-directory race |
| Realistic host cells | Six scenarios passed: C build/run, disposable Git repository, HTTP-to-SQLite, archives/hashes, concurrency and verbose subprocess output |
| Original saved Python programs | 36/36 programs, 108/108 variants passed |
| Held-out saved programs | All 333 recorded variant outcomes across 111 attempts reproduced, including original model failures |
| Changed-source quality | Clang-Tidy, strict diagnostics and GCC analyzer passed for the six changed native implementation units; repository format/Python/shell/workflow/JS checks passed |
| Nix expression syntax | Updated source, test and dependency expressions parse |

The realistic-cell helper speaks the private child protocol directly; its raw
208,914-byte noisy-command observation is not a claim about the bounded parent
result. The ordinary runtime and provider tests independently check parent-side
truncation and continuing effects. All host acceptance traffic uses private
loopback fixtures and synthetic trust roots. No new provider inference or
account access was needed for this verification.

### Source-linked formal checks

Lean 4.30.0 translated nine actual C gate functions into fixed-width BitVec
semantics and checked **37 specifications, one helper and 22 generated no-wrap
obligations**. The 6,054 representative vectors agreed across GCC and Clang with
UBSan and Lean kernel evaluation. The inherited runtime-selection proof also
passed its thirteen theorems and 371 replay vectors. **All 41 gate mutations
were rejected at the expected stage.**

The new UTF-8 lead classifier is checked against an independent range partition;
invalid/NUL leads, maximum width and boundaries are proved. The complete stream
sanitizer is additionally tested against CPython's strict decoder over every
possible lead byte and representative continuation, invalid, overlong and
truncation boundaries through both production provider wires.

The existing execution-protocol gate separately passed seven universal
obligations over its 192 input bits, 9,600 compiled cases and a non-vacuity
witness; ten existing abstract SMT obligations also passed. Its unchanged
Clang-AST translator requires the installed Clang 21 host-loader wrapper on this
machine, whereas the code-policy checker uses Clang 22 with its UBSan runtime.

These are proofs of the translated deterministic gates, not proofs of CPython,
all C memory safety, arbitrary host programs, networking, OS behavior or global
absence of deadlock. Host authority is a deliberate product choice; the proof
suite is not relabelled as a confinement proof.

## Artifact size and dependency accounting

The stripped Linux x86-64 native executable is **27,084,600 bytes** on Ares, versus
6,283,208 bytes for the earlier restricted CPython build on this host. The increase
is accepted for normal Python/host capability, not hidden by measuring a tiny
loader while excluding a separate interpreter. Linked system dependencies are
libc, libm, libstdc++ and libgcc_s; no shared libpython or separately loaded copies
of the newly bundled native libraries are required. Exact artifact/source hashes
and pin receipts are recorded in `data/receipt.json`.

The six realistic scenario wall times in `data/realistic-cells.json` are single
smoke observations, not a statistical performance benchmark or a model-efficiency
claim. Earlier model trial corpora and their denominators were left unchanged. The
saved-program replay binary predates the final descriptor-cleanup-only repair;
latest lifecycle, descriptor and provider-wire checks use the final frozen native
executable. The code-policy inputs are unchanged by that cleanup repair.

## Limits and unsuccessful runs

The inherited broad integration inventory ran while the executable was being
relinked; it is **not** a clean, single-revision full-suite success. It exposed
three missing-pin build-fixture failures, subsequently repaired and rerun. One
collective teardown failed on a directory-not-empty race; the complete group
passed its rerun. During relinking, job cases failed to start `build/tny` with
`PermissionError`; all 27 affected methods passed their recheck against a frozen
executable so a linker cannot change its mode underneath them. Their separate
terminal receipt is retained rather than overwriting the original failures.

Whole-repository `make quality` on local GCC 16 reports ownership diagnostics in
unchanged `jobs.cpp` and its C++ headers. That run is not represented as green.
The six changed implementation units pass the same unsuppressed checks in a
scoped run. Hosted CI is separate revision-bound evidence; no pending result is
called a pass. macOS/ARM, static-musl and a full Nix derivation were not run locally.

Raw synthetic receipts, before/after failures and successful gate logs are archived
in `data/verification.tar.gz`; `data/manifest.json` binds the published evidence.
Private Opus/session transcripts remain ignored under `.agent/` and are not
exported. The scratchpad records the work and the preserved diagnostic runs.
