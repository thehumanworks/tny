# Sub-agent launch ownership: experiment record

Date: 2026-09-18. Decision: [ADR 0133](../adr/0133-owned-subagent-launch-snapshots.md).

## Scope and completion criteria

This is a bounded self-improvement iteration, not an autonomous background
self-modification system. Improve a critical C ownership boundary, use the
existing harness for independent reviews, retain observable process/permission
behavior, and make verification reliable when run from inside that harness.
No new scheduler, remote-execution protocol or dependency is introduced.

| Invariant | Evidence |
| --- | --- |
| Private C++ implementation; public C ABI unchanged | Only private `core/subagent.h` changes; OS/process execution stays in `subagent.c` |
| Plan owns every retained string | Source-mutation, empty-environment and inherited-environment tests |
| Failed construction does not destroy a live plan | All 4 observed allocation indices fail closed for empty and live outputs |
| Secrets never enter argv; full snapshot wiped before free | Existing C selectors test, 36 configuration combinations, observed full-block wipe |
| Permission/provider behavior unchanged | Original selector oracle, token/account matrix, legacy behavioral mutations and child integration fixtures |
| No release allocation, leaks or escaped exceptions | Fault owner counters, ASan/UBSan, host leak checker, nonthrowing destruction assertions, C-boundary catch |
| Small artifact, measured rather than assumed | Same-version stripped release comparison below; runtime dependencies reported separately |
| No agent experiment edits the working project | All three read-only review sessions ran in temporary directories |
| Editing keeps scripts executable and rejects metadata failures atomically | Five-mode unit/CLI regressions, symlink test and refusing-fchmod fixture |
| Missing integration infrastructure cannot report success | Actual Makefile recipe tested with missing, non-executable and executable runners |

## Environment and frozen baseline

- Baseline: `14bc82802c717ed7083c208ab88b64c27318f9f8`, detached worktree.
- Host: Darwin arm64; Apple Clang 21.0.0, C11/C++20, project `-Os`/LTO/strip.
- Existing pinned tools were run with `mise exec --`; no system package install.
- Experiment root: `/tmp/tny-subagent-ownership.Tlft0F`.
- Binary comparison uses `TNY_VERSION=ownership-experiment` on both builds.
- Baseline and candidate release binaries are under that temporary root.
- Verification is fixture-based. One pre-existing provider-setup test inherited
  a configured endpoint before isolation was fixed (iteration 4). Agent reviews
  use the configured Codex account; they are not comparative live-provider benchmarks.

## Process limits

The detailed invariant/evidence table was written after implementation began;
this is an experiment log, **not** a pre-registered verification contract. The
verification-contract skill was loaded late, after an initial compound shell
lookup failed. Intercepted forms such as `tny skill show NAME` must be separate
terminal calls, not embedded in shell command chains. A direct retry worked.
No pre-implementation contract-timing or native-goal-linkage guarantee is
claimed; the active tool profile has no advertised native goal tool. Existing
ADRs are unchanged apart from their separate index. Functional evidence below
stands on the actual builds/tests, not retrospective workflow conformance.

The two launch-plan review sessions finished with stored status `done`, exit 0,
provider `codex`, model `gpt-6-astra`. Their temporary working directories and
read-only scope were kept separate from implementation. No comparative model
or swarm-performance claim follows from these two reviews.

## Iterations: hypothesis → experiment → decision

### 1. Reject an incorrect failure hypothesis

Hypothesis: buffer growth could silently truncate a launch credential on OOM.
Inspection of `buf_detach` disproved it: the sticky OOM flag already rejects the
whole value. No truncation bug or performance gain is claimed.

The useful risks were borrowed context/environment lifetimes, live rebuilds
that overwrite owning pointers, and unchecked argv capacity/count arithmetic.
An independent read-only review in the baseline worktree confirmed those risks
(session `a7298d0c1044225f`). This was a design review, not an agent-speed benchmark.

### 2. Make the launch plan an independent value

Move only construction/destruction into a private C++ owner. Use one exact-size,
wiped string block and one environment-pointer vector. Construct a candidate
before replacing the live owner. Keep C process control and the existing
credential/permission precedence rules. Four allocation points were observed
in the full instrumented fixture, including the executable-path copy.

The independent candidate review (session `26276127132f0cb9`, temporary source
snapshot) reported no actionable defect. It was static-only. Its coverage notes
led to explicit null/empty `environ` and Responses-selector tests. Impossible-
size arithmetic remains statically reviewed, not exhaustively runtime-tested.

### 3. Prove that tests detect mistakes

`test-subagent-ownership` exercises 36 token/account × permission × tool-profile
combinations, maximum argv, an 8 KiB credential, empty options, source mutation,
replacement, allocation failure, reuse, owner counts and full-block wiping.
The observer is linked only into the isolated test object.

All **11/11** scoped behavioral mutants were killed: borrowed selector,
borrowed environment, missing wipe, first-string-only wipe, destruction before
proof, leaked replaced owner, wrong permission ceiling/provider, missing key,
ambient account paired with selected token, and OOM reported as success.
Compilation failures are not counted as kills. Sources remain unchanged.

The pre-existing issue-123 mutation runner was updated for the moved source,
including both object invalidations and source hashes. Its **8/8** behavioral
mutants were killed; baseline/restored tests passed and frozen sources stayed
unchanged. Its work ran in a disposable source tree, not this checkout.

### 4. Feed verification failures back into the harness workflow

The initial `make test` inherited `TNY_TOOLS=terminal` from the executing
harness. Two fan-out unit tests failed because their fixture tools were hidden.
Separate leak-suite invocations also failed assertions before cleanup. Removing
only that variable made the unit tests and host leak gate pass.

The unit and integration runners now clear this one ambient setting. Individual
tests still set their own profiles explicitly. `test-unit` also runs selected core/search
oracles with `TNY_TOOLS=terminal` and `terminal+edit`. Production configuration,
permissions and environment propagation are unchanged.

A separate integration failure exposed `test_provider_setup.sh` inheriting a
real `OPENCODE_BASE_URL`/key rather than using its localhost fixture. It failed
with an authentication error. Reproduction used **dummy** credentials and
`http://127.0.0.1:1`, so no real service was needed. The fixture now clears both
its named-provider and default-provider credential/URL variables. The same
injected-unreachable-endpoint check then passed. This prevents that fixture
from selecting a developer's named provider by accident.

### 5. Reject a false-green gate and fix the editing tool

A later `make test` returned 0 but logged no integration groups. Inspection
showed the two edited shell scripts had become mode 0600. The running harness's
`tny edit` created private temporary files and renamed them without restoring
permissions. The old `make test` recipe silently skipped a non-executable
integration runner. **That exit-0 run is invalidated**, not counted as a pass.

The shared local editor now preserves rwx bits before atomic rename, fails
without committing on a metadata error, and still drops set-ID/sticky bits.
Scripts were restored to their tracked executable modes. The integration
recipe is now unconditional. See [ADR 0134](../adr/0134-preserve-edit-permissions-and-require-integration-tests.md).
The installed harness was not overwritten; the fix is in this project's build.

The new permission test failed before the change, then passed for five modes
under umask 0077. The CLI and symlink-target checks pass. A refusing-fchmod
fixture repeatedly verifies original bytes/mode, errno, no temporary files and
no descriptor leak. Two controlled mutants were killed by these oracles:
omitted mode restoration and ignored metadata failure. The first trial's link
failed because the experiment copied duplicate Make prerequisites rather than
Make's deduplicated `$^`; that infrastructure failure was not counted. The
corrected trial compiled and linked both mutants, then observed real assertion
failures. The mandatory-runner oracle also fails against the frozen old Makefile.

A third independent review (session `9ed33a7f15fb9ef7`, temporary source copy,
`codex` / `gpt-6-astra`, stored `done` / exit 0) found no introduced defects.
Its review was static; it did not establish runtime portability or exercise
stat failure. The final integration run is required to contain the actual
integration group executions, not just an exit code.

## Measurements

Both normalized, clean, stripped macOS binaries are **1,121,376 bytes**
(delta **0 bytes**). Both load only `libc++.1.dylib` and `libSystem.B.dylib`;
those OS runtime dependencies are not included in the artifact byte count.
No size ceiling was relaxed. These are fresh release builds, not the mutable
working build directory reused by integration fixtures.

| Median elapsed time | Baseline | Final candidate | Delta |
| --- | ---: | ---: | ---: |
| `--version` | 3.110 ms | 3.122 ms | +0.012 ms |
| `ask --help` | 3.114 ms | 3.055 ms | -0.059 ms |
| First painted PTY prompt | 2.919 ms | 2.904 ms | -0.016 ms |
| Mock TUI TTFT, 40 ms server delay | 71.8 ms | 70.9 ms | -0.9 ms |
| Mock stdin/ask completion, 40 ms delay | 152.2 ms | 152.7 ms | +0.5 ms |

Startup uses 102 samples per CLI operation and 20 PTY launches per artifact,
with three alternating batches and fresh HOME/workspaces. All median gates
passed. TTFT uses seven iterations per artifact/mode and a local strict mock.
Raw startup samples and artifact hashes are in
[subagent-launch-startup.json](subagent-launch-startup.json). No speedup is
claimed from these small differences.

**First-execution limitation:** the final candidate's `--version` maximum was
160.290 ms (p95 4.603 ms), versus baseline maximum 3.587 ms. An earlier run also
had a first-run outlier. A separate fresh-file-copy experiment reproduced slow
first execution on **both** artifacts: baseline 42–159 ms, candidate 45–223 ms;
second execution took 3.9–4.5 ms. See
[subagent-launch-fresh-copy.json](subagent-launch-fresh-copy.json). This is
consistent with host first-execution costs, but the exact cause was not traced.
All samples are retained. Median startup acceptance is not a cold-install or
tail-latency guarantee.

Snapshotting the inherited environment adds linear copy/wipe work and storage.
That is an explicit safety tradeoff, not an asserted speedup. Startup and mock
TTFT are regression checks; they do not measure sub-agent intelligence, token
use or end-to-end launch throughput.

## Reproduction

From the project root, with the existing mise toolchain:

```sh
mise exec -- make -j6 test-subagent-ownership test-subagent-mutation
mise exec -- make -j6 test
mise exec -- make -j6 quality
mise exec -- make -j6 leaks
mise exec -- make -j6 BUILD=build/leakcheck SANITIZE=0 test-subagent-ownership
leaks --atExit -- build/leakcheck/subagent-ownership/ownership-test # macOS
```

For a fresh comparison, create a temporary detached worktree at the baseline,
build both with the same compiler/flags and `TNY_VERSION=ownership-experiment`,
and run `tests/bench/bench_startup.py --baseline OLD --candidate NEW --output
REPORT.json`. Run `tests/bench/bench_ttft.py --tny BIN --repo ROOT --bench tui
--iters 7 --rpc-delay 40` and the same command with `--bench ask-stdin` for each
binary. These tools create temporary workspaces and use local mock providers.

## Verification status and limits

| Check | Final result |
| --- | --- |
| `mise exec -- make -j6 test` | Exit 0; 573/573 unit tests, two ambient-profile regressions, and **73 actual integration groups**; no failed group |
| `mise exec -- make -j6 quality` | Exit 0; format, tidy, strict warnings and script/workflow checks pass; Darwin analyzer skip remains explicit |
| `mise exec -- make -j6 leaks` | Exit 0; host gate reports zero leaks in its supported suites |
| Dedicated sub-agent owner under ASan/UBSan | Pass; 36 combinations, four allocation indices for empty/live outputs, lifetime and wipe oracles |
| Dedicated non-sanitized owner under macOS `leaks --atExit` | Exit 0; zero leaks |
| Scoped sub-agent mutations | 11/11 behavioral kills; healthy baseline passes |
| Retained issue-123 mutations | 8/8 behavioral kills; baseline/restored tests pass; original sources unchanged |
| Controlled edit mutations | 2/2 compiled mutants fail the intended mode/failure assertions |
| CLI edit / metadata-failure fixture | 8/8 tests pass, including refusing-fchmod cleanup |
| Fresh normalized release user flows | Editor and child-session suites pass; diagnostics pass with the existing Python extension host supplied in archive layout |
| Mandatory integration-runner contract | Pass; missing/non-executable runners fail and executable runner runs; old recipe fails this oracle |
| Startup / mock TTFT | All commands exit 0; startup median gates pass; first-execution limitation above |
| Independent reviews | Three read-only sessions completed; no unresolved introduced defect reported |

The final sub-agent diagnostics fixture logs an expected peer reset during
cancellation, then passes its exact-code, no-secret-echo and cancellation
oracles. Optional platform/browser/live checks can still report skips inside
the full suite; 73 executed groups does not mean every optional lane ran.
A bare temporary release binary initially failed the diagnostics fixture's
extension-event assertion (`[]`): unlike an in-tree or installed binary, it had
no adjacent Python extension host. Adding the existing `python/tny_extension_host.py`
and `python/tny_ext` under its supported `lib/tny` archive layout made that same
check pass. This was experiment packaging, not a production-code change or a
new dependency. The binary hash and startup measurements did not change.
Earlier failed environment runs and the invalidated silent-skip run are not
counted as final passes. Logs remain under the temporary experiment root;
source hashes and concise machine-readable results are in
[subagent-launch-evidence.json](subagent-launch-evidence.json).

Existing Homebrew GCC 14.4.0 also passed scoped strict syntax checks for the
C adapters and C++ owner, plus `-fanalyzer -O1` on the owner and editor. This is
still a macOS compiler check, not a Linux runtime test. Full Linux GCC analysis,
Linux/musl, MSYS, wasm runtime and Nix execution are not locally verified.
The Docker client is present but its daemon is unavailable;
Nix and emcc are absent from PATH. Existing CI platform lanes and Nix source/
target discovery include the new tests. macOS `make quality` explicitly skips
GCC `-fanalyzer`; cross-platform success is not inferred from a macOS build.

## Next bounded experiments

1. Measure real sub-agent launch throughput and peak memory across realistic
   environment sizes in temporary fixture workspaces before changing scheduling.
2. Audit another retained-data boundary, not a wholesale language rewrite.
3. Broaden fixture environment isolation only from reproduced failures. Do not
   silently delete all environment variables or alter production permission rules.

No swarm-efficiency, token-saving, SSH, wasm or cross-platform performance gain
is claimed by this iteration.
