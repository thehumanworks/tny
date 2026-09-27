# Python code-mode delivery evidence

## Scope and provenance

Worktree: `~/Projects/tny-python-code-mode`, based on updated main and the earlier
code-mode benchmark. PR #198 carries the migration; PR #197 is its historical
benchmark predecessor. The committed scratchpads record experiment setup,
corrections, unsuccessful runs and final integration work.

Runtime selection is CPython 3.14.7. The initially observed 31.9 MB shared Python
runtime was replaced with a minimal statically embedded build including frozen
bootstrap/encoding modules and the Unicode parser database. No system Python
installation, shared libpython, or filesystem standard-library tree is needed
for native code-mode cells. The optional Python extension host is separate.

## Final Unicode-complete benchmark observations

`data/release-candidate-native-artifacts.json` records same-label, same-host
native executable measurements and source/executable hashes. The label
`python-code-mode-benchmark` is for comparison only and is **not** a released
version. Fifty serial pairs per command follow five warmups; order alternates,
OS file pages are warm and host load is recorded. These are not cold-machine
or model-inference measurements.

| Artifact or median | Lua baseline | Final Python candidate |
|---|---:|---:|
| Stripped whole Tny executable | 1,401,904 B | 6,283,208 B |
| `--version` whole process | 2.704 ms | 3.437 ms |
| `ask --help` whole process | 4.443 ms | 5.564 ms |

The executable delta is 4,881,304 bytes. System C/C++ libraries remain ordinary
runtime dependencies; no external Python runtime appears in the dependency list.
The help median on this host exceeds the product's 5 ms target; it is reported,
not suppressed or mislabelled as a passing target. Latency varies with host load.

`data/release-candidate-agent-cell-latency.json` measures fifteen rotated pairs
using a synthetic loopback provider and the actual native tool-execution path.
Model/network-provider latency is deliberately excluded. Every code cell and
nested tool status must succeed before a sample is accepted.

| Median complete local task | Lua | Python |
|---|---:|---:|
| Empty cell | 16.5 ms | 27.8 ms |
| Read/search inspection | 15.5 ms | 28.5 ms |
| Read/write editing | 18.4 ms | 33.8 ms |
| List/computation | 20.4 ms | 37.6 ms |
| Three cells | 39.2 ms | 74.9 ms |

These confirm a runtime cost, not an end-to-end model speedup. Earlier
`final-native-artifacts.json` and `final-agent-cell-latency.json` were recorded
before the Unicode bootstrap fix on a heavily loaded host; they remain preserved
historical data and must not be represented as the final release measurements.

The equal-host full language/JSON embedding probe is **5,159,448 B**, including
Unicode support, versus the measured Monty probe's 6,165,632 B. The earlier
4,668,312-byte minimal CPython probe lacked the complete final implementation;
it is not presented as the final deployment size. See
`data/complete-embedding-build.json` for command and source hashes.

## Runtime compatibility and model evidence

The original model-generation corpus is immutable. Final production replay with
the Unicode-corrected runtime preserved **36/36 Python programs and 108/108
variant passes** (`unicode-production-original-replay.json`). The complete
embedding facade also reproduces those 108 passes
(`complete-embedding-post-cleanup-replay.json`). An earlier contaminated-run
observation is preserved separately, not selected as a replacement trial.

On the new held-out corpus, final production replay reproduced **333/333 recorded
pass/fail outcomes across all 111 attempts**, with no generated-code changes or
new inference (`unicode-production-heldout-replay.json`). This means the original
model failures remain failures; it does not mean 333 successful tasks.

The independent corpus auditor checks 108 sample identities, 333 typed outcomes
and 111 raw generation/code/usage receipts. The benchmark report and protocol
state task-selection bias, runtime-subset limitations and cache differences.
PyPy and RustPython were researched but not built: no numerical benchmark result
is claimed for either.

## Formal verification

The checked-in proof ledger describes the actual Clang-AST-to-Lean BitVec
translation and its trust boundary. The successful recorded Lean 4.30 run proves
36 C specification theorems plus a helper and 17 overflow obligations, and checks
6,070 GCC/Clang UBSan vectors. The Python runtime-selection gate has 13 theorems
and 371 replay vectors. All 38 negative mutations were rejected.

These proofs cover deterministic quota/admission/framing/selection predicates.
They do not establish CPython correctness, the complete JSON parser, all C memory
safety, operating-system confinement or model competence. Production tests verify
those practical boundaries separately; no model-generated success text substitutes
for executed effects.

## Completion gate ledger

The final gate receipts and exact revision are appended during completion. Until
then, this document does not claim the final PR, main or release is green.

Earlier full-suite attempts found a real fixture-linkage omission: the standalone
manifest-permission executor linked the public shared-library Python refusal stub.
That fixture must link the native interpreter while preserving public-library
refusal and every existing permission/allocation assertion. Other local failures
were an unset Go shim and a comparison-only version label. The previous GCC 16
analyzer output is not confused with the successful hosted GCC 14 quality job.

## Completion fixture and actual live production checks

The pending image-permission fixture now links Make's native interpreter graph
rather than the public-library refusal stub. Opus/high performed this scoped
correction in durable tmux; checkpoint `ad61acc` contains only the fixture change.
All eight test methods and all twenty pending matrix rows passed. A separately
relinked old graph fails the new regression and reproduces the original Python
initialization error. No production permission or allocation assertion changed.

A separate actual Tny→Codex→Python→typed-files check used `gpt-6-luna` at low
effort through the existing ChatGPT account. Its first attempt executed every
Python/file call successfully and preserved an integer above 2^53 and all nested
JSON shapes, but included a disabled row in the output ID list. The external
effect oracle rejected it despite a successful final message. One fresh-workspace
repair with the observed error corrected the filtered IDs, and all fields then
matched. Both receipts (`live-production-smoke.json` and
`live-production-smoke-repair.json`) are retained. These two requests are
integration evidence, not extra samples silently added to the frozen comparison.

The whole local quality attempt is not labelled green: the local Nix GCC 14.4
analyzer reports an existing jobs.cpp descriptor-ownership path, while hosted
GCC 14 previously passed it. The exact final hosted quality job remains the
release gate. The first repeated Lean command accidentally selected an older
Clang lacking its UBSan archive; the final code-policy check uses the installed
Clang 22 with its real UBSan runtime. Compiler setup failures do not count as
proof failures or proof successes.

## Completed exact-source gate receipts

The completed data/completion-receipt.json records full local make test exit 0
on 7c40a20: 612 unit tests, 47,455 assertions and all 101 integration groups.
Explicit platform skips remain skips. Source-linked Lean also completed on that
revision, including the 38 expected mutation rejections. Earlier failed/incomplete
logs are historical and are not substituted for these terminal receipts.

Exact-revision hosted CI, SDK and language-proof workflows subsequently completed
successfully. Their actual identifiers, event types, source SHA and conclusions
are retained in data/release-gates.json. The successful hosted quality lane does
not erase the separately recorded local Nix-GCC analyzer finding.

Main and tag v0.24.0 now contain the tested CPython implementation through an
ordinary fast-forward. The root native build was independently rebuilt at that
version and passed production execution integration; data/main-tagged-build.json
is explicitly a local binary receipt, not a claim about downloaded release assets.
No runtime logic changed during this documentation closeout.
