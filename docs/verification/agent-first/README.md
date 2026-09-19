# Agent-first harness verification

Date: 2026-09-19. Baseline: `72bafe6e6c3e863f0e3f15cfa2626d7bc140b7c8`.
Production edit implementation: `bace186`, plus this change's benchmark,
documentation and QA wiring. See [ADR 0150](../../adr/0150-agent-first-harness-and-measured-footprint.md)
and [ADR 0151](../../adr/0151-actionable-exact-edit-failures.md).

## Scope and acceptance

| Requirement | Evidence |
| --- | --- |
| Agent-first mission; no executable/wasm artifact ceiling | AGENTS/product/README/site and ADR 0150; reporting-policy tests; active Make/Nix/CI inspection |
| Fast, portable and small remain engineering goals | Size/dependency reporting retained; baseline/candidate startup measurement; no platform seam or dependency added |
| Effective context, not blindly fewer tokens | Three compiled recovery variants on a frozen corpus, including the larger-output tradeoff below |
| Exact matching and user work preserved | Tool-dispatch tests preserve failed file contents and prior undo state; shared-editor write hooks remain untouched on failure |
| Bounded, honest diagnostics | Unique first-nonempty-line advisory; UTF-8 boundary/invalid-text, truncation, tie, multiline and duplicate-match tests |
| Tests detect broken behavior | Four valid scoped mutants killed; separate mutation worktree restored and retested |
| Human can skim the outcome | Concise task summary; private reasoning and subagent deliberation are not required output |

## Selection and measured outcome

Read-only Grok 4.6 and GPT-6-astra/high scouts investigated navigation and editing
respectively. The navigation scout found hidden project files excluded by the
recursive local walk. That broader search change was deferred pending an
explicit secret/cache policy. It was **not** compared with editing on a common
live-agent task set, so this is a scoped engineering choice, not a global ranking.

The editing scout identified evidence already computed by `core/edit.c` but
discarded by `tools_fs.c`. Three alternatives were compiled and executed through
`tool_fs_execute` with real grep/read followups. Full `tools_execute` dispatch is
covered separately by the core tests.

| Arm | Recovery-result bytes | Additional evidence calls | Failed files preserved | Corrected exact retries |
| --- | ---: | ---: | ---: | ---: |
| Baseline + ideal one-hit grep | 2,398 | 18 | 22/22 | 18/18 |
| **Production bounded advisory** | **3,064** | **0** | **22/22** | **18/18** |
| Line-only advisory + one-line read | 3,748 | 18 | 22/22 | 18/18 |

The winner spends 666 more result bytes than the optimistic baseline but removes
18 scripted evidence lookups. Baseline gets an oracle-quality grep query; all
retry text and the line-only read offset are then derived from returned tool
outputs, not supplied from expected fixture values. The 18 recovery cases are
single-character typos in unique lines from six frozen repository files. Four
additional cases test ties, multiline evidence, truncation and ambiguity.

No live-agent success-rate, tokenizer saving, or edit-latency claim is made.
Request envelopes and common retry responses are excluded from the byte count.
The initial scout simulated candidate wording; those numbers were superseded by
the actual compiled comparison. Only the bounded-advisory implementation ships.

Reproduce in a Git checkout containing the baseline commit:

```sh
python3 tests/bench/bench_edit_feedback.py --out /tmp/edit-feedback.json
./build/tny-test -s core
./build/tny-test -s edit
# Run mutations only in a separate checkout, never alongside another build:
python3 tests/mutation/mutate.py --fast --focus edit-feedback --test edit_feedback
```

[edit-feedback.json](edit-feedback.json) records corpus, source and harness hashes,
compiler/build commands and actual outputs. [mutation.txt](mutation.txt) records
four valid compiled mutants, all killed by unit tests, with no survivors.

## Startup and footprint

A pre-change release was built in a detached baseline worktree, not reconstructed
from current source. Same-host mock startup, seven samples per arm and 100 ms
injected provider delay:

```sh
make -C "$baseline" -j4 release
python3 tests/bench/bench_ttft.py --tny "$baseline/build/tny" \
  --repo "$baseline" --bench ask-stdin --iters 7 --rpc-delay 100 --label baseline
python3 tests/bench/bench_ttft.py --tny "$PWD/build/tny" \
  --repo "$PWD" --bench ask-stdin --iters 7 --rpc-delay 100 --label agent-first
make size-check
```

[ttft.txt](ttft.txt): **268.7 ms baseline / 268.7 ms candidate median**. This is a
coarse startup regression observation, not edit recovery or evidence of a speedup.
Other host activity and the small sample limit interpretation.

Both stripped Linux x86_64 releases measured **1,147,664 bytes**. Dependencies:
`libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6` and the dynamic loader.
No size ceiling was used. Baseline binary SHA-256:
`367be631e0d1c357f46a39f2f6688dc3a5967bce24e44dd820b2b2c15d98165f`.
Candidate binary SHA-256:
`12008288e61177707da0bf3952d0003584c699ca6021182c624fff77268ee6c2`.

## Checks and limitations

| Check | Observed result |
| --- | --- |
| Full native unit suite, ASan/UBSan build | 602 passed, 0 failed, 0 skipped |
| Parent rerun: core / edit suites | 120 / 12 passed; 2,310 / 104 assertions |
| Compiled baseline/A/B probe | Passed; hashes verified against final source/driver |
| Scoped mutation | 4/4 killed; 0 invalid; unmodified and restored baselines passed |
| Reporting policy | Five tests passed: native and valid wasm fixtures with 6 MB and 24 MB additional data accepted; exact byte counts; missing/empty/invalid inputs rejected |
| Site generation and terminal JS tests | Passed; agent-first and historical-size regression assertions; 31 published text assets match `site/` |
| Nix expressions | `nix-instantiate --parse` passed for package/source/tests |
| Changed-file formatting and Ruff | Passed |
| Focused quality | Passed with full formatting/lint and C static analysis restricted to `src/core/tools_fs.c`; **not** a full quality pass |
| Full quality | **Failed** on analyzer findings in unchanged C++ jobs/resource code; see environment notes |
| Full `make -j4 test` | **Exit 2**: 602 unit tests passed; 81/82 integration groups passed. One unchanged Cursor test fixture fails a host GCC 16 dangling-pointer warning. Its runtime/fault group passed on an isolated GCC 13 rerun; the original aggregate is still recorded as failed. |
| `make leaks`, matching Nix GCC/libc + Valgrind | Passed: 602 unit tests and four CLI smoke commands; zero memory errors and zero definite/indirect leaks |
| Native macOS/Windows and real wasm runtime | Not executed on this Linux host; local/wasm source path is shared, SSH unchanged |
| Hermetic `nix flake check` | Not run; expression parsing and Nix-compiler leak execution do not establish a full Nix package/flake pass |
| Live-model recovery experiment | Not run; subagents performed research, implementation and independent review, not a blinded recovery benchmark |

### Environment notes and exact scoped commands

The first `make -j4 quality` attempt found missing Zsh and GCC 16 analyzer
findings. Supplying local Zsh and trying GCC 13 still reported descriptor leaks
in unchanged C++ jobs/resources. Those source hashes match baseline `72bafe6`:

- `src/core/jobs.cpp`: `66945d083d90b9e59f51bb3b6ff6b451f614af3ffb58fb9e612e2e54f44f881e`
- `src/util/resources.hpp`: `d817506523b693e1e55df7dbefa30d90f3eccee330bec6564d4663630ed36289`
- `src/util/ownership.hpp`: `14e86639b7dbd77c3ed0673c57d9eac79538f584afe2412f09de58b5e309e9f2`

The full integration sweep also exposed a GCC 16 `-Wdangling-pointer` warning
in unchanged `tests/test_cursor.c`: Greatest retains a local `err` buffer as its
failure message. That stopped `test_libtny_mutation_fault` from compiling its
Cursor fixture. The other 81 integration groups passed. Repeating that exact
Python fixture with Nix GCC 13 and a fresh `BUILD=build/agent-first-fault` passed
the runtime ownership checks, Cursor recovery probes, and exhaustive allocation
sweeps (OpenAI 293, chat 188, Cursor 159, ACP 90, ACP-WebSocket 88 active-turn
indices, plus the other runtime/toolkit/tool paths). No assertion or warning was
disabled. The repeat used the same empty HOME/XDG and credential-filtered
environment as the final leak run, `CC`/`CXX` and OpenSSL RUNPATH below, and
`MAKEFLAGS='-j4 BUILD=build/agent-first-fault'`:

```sh
python3 tests/integration/test_libtny_mutation_fault.py
```

The initial aggregate failure is not converted into a single-command pass by
this targeted rerun. All relevant runtime checks completed, but a completely
passing host `make test` / `make quality` remains outside the observed evidence.

These are observed analyzer reports, not confirmed runtime leaks or newly
introduced defects. They were not suppressed or repaired as a second feature.
The narrower verification command was:

```sh
make -j4 quality TIDY_C_SRC=src/core/tools_fs.c TIDY_CPP_SRC= \
  ZSH=/nix/store/51yi7b64mxyspaa012c5gwpmg92rs5c5-zsh-5.9.2/bin/zsh
```

The first leak attempt lacked Valgrind; the supplied Nix Valgrind then could not
instrument the stripped host loader. A separate build with matching Nix GCC/libc
resolved that environment issue without changing release flags or user settings.
The final repeat also passed with temporary empty HOME/XDG directories,
`GIT_CONFIG_NOSYSTEM=1`, and ambient `TNY_*`, API-key, base-URL and token variables
removed. The command below shows the compiler/library selection used inside that
isolated environment:

```sh
NIX_LDFLAGS_x86_64_unknown_linux_gnu='-rpath /nix/store/l0vl4dali2mvbpi30a8da1f71jl85myg-openssl-3.6.2/lib' \
make -j4 leaks BUILD=build/agent-first-nix \
  CC=/nix/store/q89hc10ykq95vsla03c2x65cyqd8y6r0-gcc-wrapper-13.4.0/bin/gcc \
  CXX=/nix/store/q89hc10ykq95vsla03c2x65cyqd8y6r0-gcc-wrapper-13.4.0/bin/g++ \
  VALGRIND=/nix/store/39iybd55afmpqlgf979ra06wj3q3wapa-valgrind-3.27.1/bin/valgrind
```

One combined foreground rerun timed out while executing the benchmark before
reaching its suite commands. It is not counted as a pass. The final benchmark
completed through the terminal task API with exit 0; core/edit were separately
rerun, and the saved result includes both production and driver hashes.

An independent GPT-6-astra/high review found no new substantive defect. It flagged
the known earlier benchmark retry-oracle issue; the final benchmark derives
retries from returned evidence and was independently rerun by the parent. Review
alone did not establish any test or platform pass.
