# October 2026 codebase cleanup audit

Date: 2026-10-18

Audit baseline: `032277e94f22666b7992abbb7f1771f4702cc69d`

Initial published code and test snapshot: `c05e2de86d0abd33730a64b379e301cde27cfefa`, tree `2932ce24edffe3ecd3fb05fe274cbe594337a1b7`

This audit removed unused implementations, stale state, duplicate helpers and
verification machinery with little continuing value. It also corrected concrete
integer, buffer, history, browser-startup and terminal-randomness defects.
Compatibility readers, public ABI contracts, platform seams, frozen experiment
evidence and substantive production proofs were retained where removal would
weaken the application or require a separate migration.

The register contains **94 candidate dispositions: 44 implemented, 26 retained
and 24 deferred**. These are 94 reviewed facets, not 94 independent changes:
some IDs cover the source, tests, documentation and build hooks for the same
change. The prefixes identify the four audit streams.

| Stream | IDs | Implemented | Retained | Deferred |
| --- | --- | ---: | ---: | ---: |
| Repository, build, SDK and integration | M01–M22 | 9 | 5 | 8 |
| Core runtime and native backends | R01–R25 | 12 | 10 | 3 |
| CLI, TUI, terminal and browser surfaces | S01–S30 | 17 | 5 | 8 |
| Formal checks and invariant maintenance | INV01–INV17 | 6 | 6 | 5 |
| Total | 94 dispositions | 44 | 26 | 24 |

## Method and scope

Four parallel audits read the applicable product, architecture, implementation,
platform and component contracts before changing code. They traced callers,
assignments, generated artifacts, persisted formats and build consumers;
compared apparent duplication against ownership and error-handling contracts;
and used focused regression, negative and mutation controls where behavior
changed. Agents cross-reviewed each other's changes before integration.

The baseline inventory included 220 production runtime C/C++/header files
(70,912 lines); CLI, TUI, browser and terminal sources (22,144 lines excluding
vendored JavaScript); SDK/ABI, build and documentation surfaces; the Rust GUI
and its process adapter; and all five Lean packages, 19 Lean files, translators,
golden fixtures and the two standalone SMT models. These are scope inventories,
not independent totals to add together. Large files and low reference counts
were investigation leads, not sufficient reasons to remove code.

Paths below are repository-relative; deleted paths refer to the baseline.
**Implemented** means the concrete change and its focused verification were
completed, not that every supported platform was exercised. **Retained** records
why the existing code or evidence is useful. **Deferred** records a worthwhile
candidate whose prerequisites or remaining behavioral risk make it a separate
change. Provider checks used local mocks and synthetic credentials, without
live inference.

The published commits preserve the tested Git trees, although their commit IDs
changed during publication. Relevant checkpoints are:

| Scope | Published revision | Original local revision |
| --- | --- | --- |
| Integrated invariant changes | `2f0506d646531a80240b8a84fb65b7d6e76b141c` | `351a3e8ed09c554d6b41c637691f2f0f585a4436` |
| ABI and SDK verification inputs | `8cb952f87df79dc9e8fb34bddb917c4412d0ecfe` | `6846104e5ccb6b85b05dd74b32599628d77a7c35` |
| Full unit, Valgrind and aggregate integration inputs | `efffe74ac4700f3234197f622440d518a82c3ea8` | `8f14b968e9142c6f6e272d16ef51e515e9d43e3b` |
| Original production checkpoint | `385a8475dddfd8b5ba70c8bc6742bb01903029e5` | `1071d26d600fb453e5ad17fe4a1006ee2479e92d` |
| Pre-publication test-only jobs synchronization | `c05e2de86d0abd33730a64b379e301cde27cfefa` | `3412ba39cc6c24d82fa95b8f3272dccb76d8bdca` |

## Change footprint

Compared with the audit baseline, the initial `c05e2de8` snapshot removes 510 net
lines from the runtime, terminal and browser implementation. The separately
required Pages mirror removes another 261 net lines. Focused tests and this
audit add verification and review material outside the shipped runtime.
These are source-line counts, not binary-size or performance estimates.

| Scope | Changed files | Added lines | Deleted lines | Net removed |
| --- | ---: | ---: | ---: | ---: |
| `src/`, `tnytty/src/`, `site/assets/` | 44 | 353 | 863 | 510 |
| Required `docs/assets/` mirror | 2 | 40 | 301 | 261 |

The later M18/M21/M22 changes leave both scoped counts unchanged. They change
the conformance tool, desktop test/workflow setup and shell reaper instead;
the shell production edit has 11 lines added and 11 removed.

## Selective Lean retirement

[ADR 0188](../adr/0188-focused-verification-maintenance.md) records the decision
to retire the two benchmark-specific Python-to-Lean translators and their
theorems, including one complete benchmark package. Their subjects were the
23-line language benchmark policy and the 35-line runtime-selection policy,
rather than code in the shipped harness. The replacement tests call those
actual policy functions and run in the ordinary offline suite.

The language checks preserve the complete eight-case acceptance table and the
existing 2,048-case promotion cross product. Added controls cover strictness at
equality, adjacent integers beyond binary64 and uint64, and 12 mutations. The
runtime checks cover 5,832 labeled decision combinations, 1,296 exact
`Fraction` ratio vectors, 27 large-integer threshold cases, 1,536 positive
scaling cases, archived evidence and 17 mutations. Mutation runs first require
the unchanged control to pass, then require each weakened copy to fail.

This is a deliberate change in the kind of evidence. The finite tests do
**not** preserve the former all-integer theorem or the translators' AST purity
whitelist. They do not prove experiment quality. They provide direct regression
coverage of the unchanged benchmark decisions without maintaining a second
policy implementation.

The preregistered policy files, recorded results, raw receipts and 11 retained
golden files remain byte-for-byte unchanged. The policy hashes are:

| File | SHA-256 |
| --- | --- |
| `tests/bench/code_mode/policy.py` | `d402679402a5f13ace64de82ca4a958043e6129bb444f7ee1e9350a24bb2374a` |
| `tests/bench/python_runtime/policy.py` | `04819b8fe259e9a9d45b8eb8e229d03cc4f331c6d34afb595335cf4ce505bae5` |

The source-linked production C policy gate remains. It translates nine actual
C gates with exact integer widths and guarded overflow obligations, and retains
the independent specification, compiler comparisons and 30 negative C mutations.
ACP, dictation and GUI Lean packages also remain: their general specification
theorems and exported examples protect useful ordering, normalization,
lifecycle, model-selection, work-directory and markdown behavior. Their C/Rust
replay checks establish finite conformance to those models; they do not prove
every parser, process or UI path. Execution-RPC C-linked Z3 checks and the shell
model also remain.

The shared `tests/formal/check_lean_golden.py` now runs a full `lake build`,
exports into a temporary directory and compares the complete relative file set
and exact bytes. It fails for stale, missing or extra output and for build or
export failure, without repairing the checkout. Make, CI and GUI hooks use the
same helper; Nix retains Lean for the production C gate.

### Observed check cost

The old checks used the exact baseline inputs. These are wall-clock observations
of one sequential run per old/new check on the same shared Linux host while
other work was running, not an isolated benchmark or an application-performance
claim. The original environment used Lean 4.30.0.

| Benchmark-policy check | Former verification | Replacement tests |
| --- | ---: | ---: |
| Code-mode language | 7.444 s | 0.214 s |
| Python runtime | 119.129 s | 0.580 s |
| Combined | **126.573 s** | **0.794 s** |

## Repository, build and SDK register

| ID | Disposition | Evidence and action or retention reason | Verification or next gate |
| --- | --- | --- | --- |
| M01 | Implemented | `sdk/schema/check.py` regenerated tracked outputs before checking them, potentially repairing the condition it should reject. It now generates temporary output and reports stale or missing artifacts without modifying them. | Current, stale and missing-output subprocess controls in `tests/integration/test_event_schema.py`; schema and SDK checks below. |
| M02 | Implemented | Five `.cursor/rules/` files required removed transports, prohibited authorized private C++ and imposed an obsolete size ceiling. Replace them with a single pointer to canonical `AGENTS.md` and component contracts. | Compare ADRs 0114, 0150, 0152 and 0164; retain the existing size-policy checks. |
| M03 | Implemented | `AGENTS.md`, `.mise.toml` and `Makefile` repeated obsolete formatting/lint commands. Point fallback instructions at the maintained pinned commands in `docs/ci.md`. | `tests/integration/test_toolchain_pins.py` and the completed quality gate. |
| M04 | Implemented | Benchmark-only Lean hooks survived in Make, the offline workflow and Nix. Route them to direct policy and mutation tests; preserve the CI check identifier and the production Lean dependency. | Offline checks and workflow validation; Nix remains a separate unexecuted local gate. See INV01–INV03 and INV07. |
| M05 | Implemented | ACP, dictation and GUI export recipes wrote tracked goldens; `git diff` also missed untracked output. Wire the shared temporary-output checker into Make/CI and include it in GUI workflow path filters. | Seven negative controls and three real package builds/exports. See INV04. |
| M06 | Deferred | The 1,655-line root `Makefile` couples native, Wasm, PIC and fault-library lanes through variables; Nix and source fixtures also name it. Fragmentation needs explicit ownership and include closure. | Preserve default targets, source classification and all supported build lanes before and after extraction. |
| M07 | Deferred | The baseline documentation inventory contained 977 files, about 20.9 MB, including repeated logs and baselines. Many are hash-pinned evidence or active test inputs. | Build a reference/hash inventory and durable evidence manifest before archiving or deleting any material. |
| M08 | Retained | Repeated repositories and solutions in `tests/bench/harness_bench/` are deliberate independent frozen experimental inputs. Deduplicating them can alter the corpus or invalidate recorded results. | Preserve corpus hashes; use an explicit experiment migration if the corpus changes. |
| M09 | Retained | The site/documentation mirror and compiled Wasm files are inputs to the current `main:/docs` Pages deployment. Their duplication follows a deployment contract. | A separate deployment migration must change repository settings/source contracts and verify the published browser. See S27. |
| M10 | Retained | `abi/`, `include/tny/` and `src/lib/tny.c` retain ABI 0 and older struct prefixes for published consumers and frozen probes. Internal call counts do not establish obsolescence. | Require an explicit compatibility release/migration and legacy fixtures plus all ABI manifests. |
| M11 | Deferred | The roughly 2,000-line `src/lib/tny.c` combines initialization, prefix validation, ownership and session/event adapters at the public boundary. A split can obscure ABI-sensitive lifetimes. | Map module ownership, then run manifests, legacy compatibility, ownership failures and SDK conformance before and after. |
| M12 | Deferred | Python, C and Node option/event validators resemble each other but encode different lifetimes, UTF-8, integer and error contracts. | Define the genuinely shared structural schema before generating validators; retain language-specific ownership and conformance checks. |
| M13 | Deferred | Schema-generated fixtures do not imply that production bindings can be generated without changing compatibility behavior. Unknown events and optional fields need explicit parity first. | Establish cross-SDK acceptance/default/error behavior, then evaluate production generation against those contracts. |
| M14 | Deferred | Large Python and TypeScript workflow validation, execution and context modules contain cancellation and provenance rules, not just repeated plumbing. | Extract narrow responsibilities with cross-language ordering, cancellation and cleanup-failure fixtures. |
| M15 | Retained | The pthread mutex owner in `src/lib/custom_tools.cpp` exposes explicit initialization failure and lifetime behavior. Replacing it with `std::mutex` changes exception/OOM behavior behind a C ABI. | Demonstrate initialization-failure, asynchronous invalidation and leak equivalence before changing the owner. |
| M16 | Retained | Extension-host Python 3.9 support differs from the SDK's Python 3.10 floor. Blanket typing modernization would silently change supported versions. | Keep supported-version checks and avoid cosmetic changes that raise either floor. |
| M17 | Deferred | `scripts/cpython_runtime.sh` is over 400 lines, but its pinned source, static archive and standard library implement a product requirement. Build cost does not make those features obsolete. | Evaluate cache and per-library isolation with reproducibility, artifact-identity and multi-target checks. |
| M18 | Implemented | `sdk/conformance/check.py` used removable assertions to validate the committed executable contract. Replace them with explicit failures and useful diagnostics, preserving all 18 predicates and normal success output. Adapter report validation was already explicit and is unchanged. | Baseline corruptions falsely passed under `-O`/`-OO`; 24 malformed variants now fail in all three Python modes. The full nine-test conformance target and pinned Ruff checks passed. Existing Make discovery and Nix filesets include the new controls. |
| M19 | Deferred | `tasks/doing/` and `agents/tasks/` mix completed work with historical plans. Moving or deleting them from directory names alone would lose context. | Match plans to commits/issues, classify current work, and preserve the resulting history and references. |
| M20 | Implemented | Two `tests/integration/test_jobs.py` fixtures raced valid production ordering: a 0.2-second response hold matched the poll interval, and terminal metadata could precede owner-lock release. Use an explicit provider-response barrier and the existing owner-lock probe. | Both affected cases passed in 8.741 s against the frozen integrated binary. Privacy checks now require nonempty live argv and final success; barriers release in `finally`. Test-only change, no scheduler edit or timeout increase. |
| M21 | Implemented | The desktop workflow omitted Linux Fontconfig development metadata, while two unchanged index tests used symlinked macOS temporary roots even though application workspace inputs are canonicalized. Install the required package and canonicalize only fixture roots. | Original physical-root controls: two passes; original symlink-root controls: the exact two macOS failures; corrected fixtures: two passes with all symlink/exclude/boundedness assertions retained. Pinned actionlint, Rust formatting and the Fontconfig probe passed. Full desktop checks remain separately recorded below. |
| M22 | Implemented | `shell/tny-workflows.sh` could overwrite a real result when a worker finished between the initial result-marker check and the liveness probe. Merge duplicate reaping branches, wait, recheck publication, and remove both process records in the same pass; synthesize a crash only when no result was committed. | Deterministic real-worker controls reproduce result loss on unchanged source in Bash and Zsh. The corrected full suites preserve success, byte-limit diagnostics/no provider launch, and true-crash refusal; require exactly one reap and immediate record cleanup. Both shell syntax checks, pinned ShellCheck/shfmt and independent review passed. |

## Runtime register

| ID | Disposition | Evidence and action or retention reason | Verification or next gate |
| --- | --- | --- | --- |
| R01 | Implemented | Four `acp_send_*` helpers in `src/backends/acp/acp_wire.c` had no callers. Remove their definitions/declarations; live `ac_tx_*` paths already implement sticky OOM handling. | Whole-repository consumer scan, strict builds and ACP fixtures. |
| R02 | Implemented | `acp_blocks_to_text` and its sole `block_text` helper were unused; active content handling lives in `acp_events.c`. Delete the obsolete conversion path. | Reference scan, compilation and ACP behavior checks. |
| R03 | Implemented | `src/core/image_export.c` retained unused input/detail wrappers after execution moved to owned plans. Remove the obsolete contracts and preserve the owned permission/execution API. | Image-service and retained-plan fixtures; public behavior remains exercised through the owning path. |
| R04 | Implemented | Five private helpers in `team_control.c`, `tasks.c`, `extensions.c`, `runtime.c` and `dictation_verify.c` had no consumers: a sensitivity predicate, name joiner, status accessor, background wrapper and admissibility wrapper. | Remove only dead wrappers; retain their live underlying permission, listing, diagnostic, continuation and verification behavior. Check references, formal extraction and compilation. |
| R05 | Implemented | `acp_id_text` formatted integer IDs with signed `%lld`, corrupting `UINT64_MAX`; string escaping truncated embedded NUL. Serialize the JSON value with `jwrite_val`. | Integer extrema, escaped NUL, missing and null cases; baseline corruption reproduced and fixed cases passed under sanitizers. |
| R06 | Implemented | `src/json/json.c::jget_int` cast out-of-range real values to `int64_t` and reinterpreted oversized unsigned integers. Check bounds, retain valid real truncation, otherwise return the caller's default. | Extrema, adjacent floats, fractions, nonnumeric and missing cases. Baseline float-cast UB reproduced; 205 call sites motivate the integrated gates below. |
| R07 | Implemented | `str_trim` in `src/util/util.c` repeatedly measured and shifted the string for each leading whitespace byte. Scan both boundaries and perform at most one move. | Preserve original pointer and interior bytes; cover all six C whitespace characters, empty/UTF-8 input and a 64 KiB prefix. No throughput claim. |
| R08 | Implemented | Three front-consumption sequences in `src/core/acp_bridge.c` duplicated `buf_consume`. Use the existing helper. | Preserve count and terminator behavior; bridge oversized-write, error and cancellation fixtures. |
| R09 | Implemented | `mcp_secret_free` in `src/mcp/mcp_http.c` duplicated the existing secure wipe/free primitive. Use `secure_free` with the same NULL contract. | MCP tests and sanitizer checks; no change to credential lifetime or diagnostics. |
| R10 | Implemented | Event JSONL, runtime and runner paths repeated 14 event names and five stop names. Centralize private inline mappings in `src/core/events.h` while preserving runner-specific aliases. | Exact case/default comparison, ABI enum assertions, current schema, nine event-registry checks and behavioral fixtures. |
| R11 | Implemented | `src/core/backend.h` and `src/mcp/mcp.c` described obsolete prewarming and removed Cursor behavior. Document lazy runtime connections and the actual subprocess contract. | Preserve the asynchronous-signal-safe child rule for threaded embedders; review against the live startup path. |
| R12 | Implemented | `tests/test_net.c` contained two unused Connect-collector typedefs and a stale heading. Remove only those declarations. | Definition-only search; retain the actual network and split-boundary tests. |
| R13 | Deferred | The 7,364-line `src/core/jobs.cpp` joins transactions, reservations, DAG handling, CLI output, restore and scheduling under private ownership rules. | Begin with pure validation/formatting after mapping locks; require crash/retry, team, reservation, mutation, fault and leak checks. |
| R14 | Deferred | The 3,016-line `src/backends/openai/openai.c` shares `oa_impl` across requests, retries, streams, tools and continuation. A broad split risks ownership and cancellation behavior. | Preserve existing decoder/owner boundaries; add wire, split, OOM, stale-replay and cancellation evidence plus size/latency measurements. |
| R15 | Retained | Persisted native parked continuations, runner restore and provider restart state have consumers across process and version boundaries. A small direct call count is misleading. | Require checkpoint-version migration and restart-continuity evidence before removing readers or state. |
| R16 | Retained | JSON validators in team control, jobs, swarm manifests, execution protocol and TNYJEV use different depth limits, node/member caps, closed fields and nullable/forward-compatible rules. | A shared parser first needs explicit per-domain policies and independent acceptance/rejection parity. |
| R17 | Deferred | `src/net/url.c` uses `atoi` for ports, truncates paths and lacks IPv6 authority parsing. Tightening it can change accepted custom endpoints. | Define URL, IPv6, Unix and query grammar; test malformed/boundary inputs across native and Wasm consumers before replacing the parser. |
| R18 | Retained | `src/core/shellwords.c` handles argv and quoted spans; `src/core/shlex.c` conservatively classifies authority, assignments, comments and dangerous options. They recognize different languages. | Preserve independent permission counterexamples before considering a common scanning primitive. |
| R19 | Retained | Codex and Grok have a small duplicate `print_oauth_error`, but consent, device and refresh flows differ. Exporting a tiny helper would add coupling without a useful boundary. | Revisit only with a dedicated authentication service and synthetic flow fixtures. |
| R20 | Retained | Image service/export persistence adapters share serialization and atomic I/O already, while owning different status and timestamp contracts. | A third meaningful owner may justify extraction; preserve current ownership and persistence tests. |
| R21 | Retained | Domain terminal/string/hex/base64 predicates are not interchangeable. Image decoding rejects padding aliases and truncation that utility code may accept. | Define a common type/acceptance contract and negative parity before merging decoders. |
| R22 | Retained | Named error codes in `src/core/jobs.h`, `src/util/image_transform.h` and `src/backends/acp/acp_wire.h` include reserved protocol/ABI enum values. | Establish external compatibility requirements before removal; token counts cannot prove that values are unused. |
| R23 | Retained | Legacy MCP imports/initialization, ACP selectors/configuration and Codex OAuth remain active product paths. | Require migration/usage evidence and import, login and profile parity before deprecation. |
| R24 | Retained | Private C++ request, turn, event, checkpoint and resource owners encode OOM settlement and lifetimes behind the C ABI. Reducing file count would not simplify those guarantees. | Keep fault and ownership tests; extract only where a clearer owner is demonstrated. |
| R25 | Retained | Explicit Wasm unsupported seams and `src/util/tny_poll.c` preserve error contracts and Asyncify pseudo-fd behavior. Replacing them with direct `poll` can livelock. | Require native/Wasm behavioral equivalence and platform fixtures before changing those seams. |

## Surface register

| ID | Disposition | Evidence and action or retention reason | Verification or next gate |
| --- | --- | --- | --- |
| S01 | Implemented | `site/assets/term-core.js` retained unused `SseParser`, `toResponsesInput` and `ResponsesTurn`. Production `term-wasm.js` uses the native Wasm provider loop. Remove the duplicate decoder and obsolete tests. | Retain actual intake/layout cases; the site architecture gate now rejects typed Responses events as well as endpoint strings in JavaScript. |
| S02 | Implemented | The same file retained unused AES-GCM/base64 vault code, `WEB_CMDS`, secret-draft parsing and display redaction, referenced only by obsolete tests. | Delete only the retired path. Live URL secret intake, sanitizers and native credential handling remain; eight retained Node cases pass. |
| S03 | Implemented | `ENV_MAP` duplicated a subset of `PARAM_MAP` and several exports had no consumers. Keep one bootstrap vocabulary and internal merge/strip helpers. | Query/hash precedence, stripping and named-provider tests; consumer search. |
| S04 | Implemented | `tests/integration/test_site.py` required retired vault and SSE helpers. Replace those stale assertions with actual secret-intake and architecture checks. | Keep `docs/assets/term-core.js` byte-identical to the site source; syntax, Ruff, mirror and generated-site integration passed. |
| S05 | Implemented | The 15-line `src/tui/tui_prewarm.c` was an invalidation shim with phantom warmup-thread claims. Move its operations into `tui_runner.c` as defer-rebind/invalidate helpers and update exact mutation-registry references. | Preserve predicates and active-turn deferral; TUI compilation and fixtures. No actual prewarming thread was removed. |
| S06 | Implemented | `bk_adopted` was never assigned true. Delete the field, false resets and unreachable prewarmed-host retry. | Assignment scan and startup/retry tests; the working detached-runner retry remains. |
| S07 | Implemented | `prompt_text` was an owned prompt copy that was initialized, assigned, cleared and freed without a reader. Remove the duplicate allocation/state. | Turn, steering and interrupt fixtures. Avoid claiming a measured performance improvement. |
| S08 | Implemented | `perm_opts` was declaration-only; the TUI's `perm_id` was never assigned, making its NULL condition redundant. Remove the dead state/check. | Preserve real approval state and event permission IDs; approval, composer and background-key fixtures. |
| S09 | Implemented | `src/cli/cmd_provider.c` rejected `--api-key` before assignment, leaving raw `api_key` NULL; every prompt used `secret=false`. Remove unreachable raw-key, termios and echo plumbing. | Preserve environment-variable-only setup; native provider commands, PTY wizard and strict compilation passed. |
| S10 | Implemented | `optimise`, `score` and `choose` had parallel command-recognition logic. Add them to `cli_is_command` and remove the special condition; also remove the `login_grok(ctx)` forwarding wrapper. | Help, flag interception and synthetic authentication paths; no live login. |
| S11 | Implemented | `tnytty/src/main.c` and `tnytty/src/ui/gui.c` duplicated `parse_listen`. Use shared `tt_parse_listen` in `util/tt.c` and `util/tt.h`. | HOST:PORT grammar unchanged; valid, invalid and boundary C tests plus release/debug/strict builds and sanitizers. |
| S12 | Implemented | Terminal buffer addition/doubling could overflow; a zero-length append could reach `memcpy(NULL, 0)`. Check arithmetic before growth and skip empty copies. | SIZE_MAX, sticky OOM, preserved bytes, ordinary growth and NUL-termination controls under ASan/UBSan/LSan. |
| S13 | Deferred | The roughly 930-line `src/cli/cmd_misc.c` mixes tasks, status/allowance HTTP, models, providers, workspace, MCP, usage and auth. | Extract cohesive feature adapters only if cheaper to maintain; preserve bodies, stdout, exit behavior and mutation registration. Size alone is insufficient. |
| S14 | Deferred | Ordinary and ephemeral status rendering look similar, but ordinary status includes self-improvement, plan metadata and weekly allowance paths absent from ephemeral mode. | Define allowed I/O and field policy, then use synthetic-profile snapshots; do not introduce conversation loads or subscription queries into ephemeral status. |
| S15 | Deferred | CLI and MCP stdin loops have different byte limits, TTY, empty-input, NUL and cancellation semantics. | Consider a bounded-reader primitive with explicit policies and split/EINTR/EOF/NUL/exact-cap/cancellation tests; avoid a generic command parser. |
| S16 | Implemented | `src/tui/tui_hist.c` loaded the entire append-only history before trimming and did not bound additions. Stream records, decode in place and keep the latest 500 entries in memory. | Preserve disk history/format, duplicate/no-write behavior and existing entries on allocation failure. Five actual-source tests, 2,043 assertions; existing TUI and PTY suites passed. |
| S17 | Retained | `push_trunc` and `tui_push_ansi` differ deliberately: the ANSI scanner continues past the width limit to preserve SGR resets. Merging their loops can leak terminal style. | Any shared scanner needs split-UTF-8, reset and overlay-budget parity. |
| S18 | Implemented | `term-wasm.js` treated throwing sanitizers as empty-value predicates and could reject startup without a usable retry. Validate startup atomically, retry invalid interactive input and handle non-Error module rejections. | Thirteen actual-bootstrap VM tests; invalid bases must never reroute retained keys to a default host, and diagnostics must not disclose the key. |
| S19 | Implemented | The chosen `boot.model` was discarded at the Wasm boundary. Forward it as `OPENAI_DEFAULT_MODEL`, matching `src/core/config.c`. | Actual module-boundary tests pass. A real-Wasm selected-model banner assertion was added; its browser gate was not completed locally. |
| S20 | Deferred | Terminal CLI/GUI dimensions use `atoi` while HTTP enforces 1–1000. The shared listener parser does not resolve this numeric-policy mismatch. | Define supported dimensions before tightening inputs; check malformed/overflow values and PTY/framebuffer propagation. |
| S21 | Implemented | Baseline `tnytty/src/util/tt.c::tt_rand_hex` fell back to time/PID data. New `util/random.c` returns checked bool/errno, retries OS interruption/fragmentation and fails closed. All three callers check failure; listener errors return to the owning session cleanup. | Final 155 C tests/1,900 assertions passed with ASan/UBSan/LSan; seven CLI fault scenarios also passed. Entropy and occupied-bind failures prove an already-owned HUP-ignoring child is killed/reaped. No product fault switch. |
| S22 | Deferred | Terminal broker synchronous writes/receives restart the same five-second poll budget after every fragment, allowing a slowly progressing peer to exceed a total deadline. | Introduce a monotonic total deadline with stalled/fragmented-peer fixtures; preserve cancellation and side-effect retry behavior. |
| S23 | Deferred | The 1,128-line `tnytty/src/ui/gui.c` mixes flags, panes/tabs, persistence, snapshots, input and painting. | Extract platform-free topology/snapshot decisions first, preserving process ownership; require unit checks and real macOS attach/detach/close validation. |
| S24 | Implemented | `tnytty/docs/product.md` compared against a removed root 2 MiB ceiling; the implementation plan described phase 1 as current despite shipped phase 3.6. | Correct present-tense claims against ADR 0150, preserving append-only ADR history and the sibling's own footprint goal. |
| S25 | Deferred | The 3,657-line `gui/src/main.rs` joins provider picking, filesystem indexing, JSON view models, workers and callbacks. | Extract pure picker/index/view-model responsibilities first; headless Rust tests, production goldens and GUI build are prerequisites. Add no extra async runtime. |
| S26 | Deferred | `gui/src/main.rs::git_output` and `gui/src/bridge.rs::run_process` both bound process output/deadlines but differ in byte formats, errors and lifetimes. | Reassess a shared process primitive after S25; preserve no-shell argv, inherited-pipe deadlines, retries and JSON/binary/descendant timing cases. |
| S27 | Retained | Committed site/docs HTML, CSS, JavaScript and Wasm binaries serve the current Pages source contract. Deleting the mirror alone would break deployment. | Migrate repository settings and artifact publishing together, with deployed-browser verification. See M09. |
| S28 | Retained | `src/main.c` dispatches help/version, no-runtime commands and auth repair before runtime initialization, avoiding unnecessary provider/configuration I/O. | Preserve ordering and quick-start/invalid-config behavior. S10 only consolidates name recognition. |
| S29 | Retained | TUI dictation and optimization lifecycles resemble each other but have different readiness, cancellation, insertion and normalization semantics. A generic adapter would add state. | Share only after demonstrating a simpler contract and behavioral parity. |
| S30 | Retained | Terminal VT, layout, rendering, broker and session-buffer tests protect arbitrary split boundaries, atomic snapshots and detach/kill behavior. | Keep all baseline 149 tests/1,794 assertions; add focused regressions rather than reducing meaningful assertions to lower test counts. |

## Invariant and formal-check register

| ID | Disposition | Evidence and action or retention reason | Verification or next gate |
| --- | --- | --- | --- |
| INV01 | Implemented | Two benchmark Python-to-Lean translators duplicated verification machinery around 58 lines of unchanged policy. Retire them and their benchmark theorems. | Direct policy tests and archived replay replace them; frozen evidence is unchanged. The finite-versus-general guarantee change is explicit in ADR 0188. |
| INV02 | Implemented | Language acceptance/promotion needed direct controls for individual guards, strict comparisons and integer precision. | Retain all eight acceptance and 2,048 promotion cases; add large adjacent integers, a passing mutation control and 12 rejected policy mutants. |
| INV03 | Implemented | Runtime-selection verification now exercises the actual policy rather than a second translated decision implementation. | 5,832 decision combinations, 1,296 Fraction vectors, 27 large-integer thresholds, 1,536 scaling cases, archived replay and 17 rejected mutants. |
| INV04 | Implemented | Golden checks exported into the checkout and used a comparison that could miss extra untracked files. Add `check_lean_golden.py` for full builds and temporary exports with exact path/byte comparison. | Seven negative controls, including build/export failures; actual ACP, dictation and GUI packages passed. M05 supplies hooks. |
| INV05 | Implemented | An ACP SMT query assumed the requested ID absent, then contradicted that assumption. Its claim that an unlisted ID could not become selectable was misleading after ADR 0181. | Remove only that query. Three catalog-provenance queries remain; `Model.lean` and subprocess fixtures cover adapter-confirmed unlisted requests. |
| INV06 | Implemented | Formal documentation omitted the production C package, described eight rather than nine current gates and blurred historical timeouts/results with current configuration. | Document all nine gates and the current 600,000 ms timeout; mark historical reports as historical without rewriting their evidence. |
| INV07 | Retained | Nix still runs `verify-code-policy` after the benchmark target. Removing Lean from its dependencies would break a supported production verification gate. | Retain Lean; retire only the obsolete benchmark hook. Nix execution remains an external gate. |
| INV08 | Retained | The production C policy proof provides general width, conversion and guarded non-overflow guarantees that 6,054 agreement vectors alone cannot establish. | Keep translator, independent specification, compiler comparison and 30 negative C mutations. |
| INV09 | Retained | Dictation proofs cover general correction decomposition, dynamic-program equivalence and fail-open lifecycle properties. | Keep independent models, goldens and actual implementation replay; finite replay is not a replacement for the theorem. |
| INV10 | Retained | ACP proofs cover minimum ordering, fresh authority, revalidation and catalog-independent requests. Valid model states do not prove every C state-construction path. | Retain the general specifications together with C subprocess/bounded replay checks. |
| INV11 | Retained | GUI work-directory and markdown round-trip proofs plus Rust golden replay protect real behavior, while intentionally excluding asynchronous UI plumbing. | Keep the proofs and replay; still require ordinary GUI and platform validation for that plumbing. |
| INV12 | Deferred | C-to-Lean and C-to-Z3 translators differ in accepted syntax, types, signed conversions and overflow semantics. A generic framework can add more maintenance than it removes. | Establish accepted/rejected-input and negative-control parity before sharing translation infrastructure. |
| INV13 | Deferred | An ACP `minimum_accepted` proof took about 247 s cold. A `by decide` experiment hit a String parser failure; an identity-simplification attempt still exceeded 60 s. | Retain the exact theorem. Profile/prototype separately; no failed proof experiment is included in the change. |
| INV14 | Deferred | Independent C mutations could run with bounded parallelism, but concurrent Lean checks created memory pressure during this audit. | Measure peak memory and elapsed time first; preserve passing-control-first behavior and precise rejection-stage assertions. |
| INV15 | Deferred | `gui/proofs/Proofs/Effort.lean` has no exported golden linkage; picker behavior is linked by ordinary tests and review. | Consider finite catalog checks against the real selector, with explicit scope; do not imply equivalence for arbitrary catalogs. |
| INV16 | Deferred | ACP/dictation replay readers use minimum row counts, while the new regeneration check enforces exact fixture sets. More strict parsing has limited immediate benefit. | Add malformed-parser controls when changing the format; avoid tests that merely duplicate current row counts. |
| INV17 | Retained | The small `tests/formal/check.py` Z3 launcher fails closed on diagnostics, satisfiable/unknown results and missing tools. Six shell obligations complement concrete tests. | Keep it and its obligations; replacing 22 clear lines with a framework or deleting the check has no demonstrated benefit. |

## Focused verification completed

These results describe observed runs on the recorded source inputs. They are
separate from the aggregate integration gate and from platforms not executed
locally.

| Area | Observed result | Scope and limits |
| --- | --- | --- |
| Native runtime boundaries | 30 tests, 2,500 assertions passed under ASan/UBSan using actual Makefile debug objects. | Baseline UINT64_MAX ID corruption and out-of-range float-cast UB were reproduced; fixed behavior returned the proper ID/default without sanitizer errors. |
| Changed runtime compilation | All 14 changed production translation units passed GCC 14 and Clang 18 strict warnings and clang-tidy. | Includes the changed private C++ runner; full compiler lanes are listed separately below. |
| Event and schema contracts | All 14 generated event cases current; nine event-registry checks passed. Schema current/stale/missing controls passed. | Preserves event/stop names and runner aliases; SDK/ABI evidence remains separately scoped. |
| Terminal core and cleanup | 155 C tests, 1,900 assertions, zero failures/skips under ASan/UBSan/LSan; seven CLI fault scenarios also passed. | Includes interrupted-read success and entropy/bind failure cleanup of an already-owned HUP-ignoring child. Release/debug/strict builds and benchmark-helper link passed; no fault gate disabled. |
| TUI history | Five actual-source tests, 2,043 assertions passed with ASan/UBSan/LSan; existing TUI suite 66 tests, 2,695 assertions passed. | Local allocator interposition exercises allocation failure without a production hook; disk history and latest-500 memory behavior covered. |
| TUI and setup integration | All 24 named PTY scenarios passed, including 36 settings combinations and three PTY sizes. Provider-setup and generated-site integrations passed. | Covers wizard, resume, approval, history, steering, fullscreen and clipboard behavior with synthetic/local inputs. |
| Browser helpers and bootstrap | Eight retained helper tests and 13 actual-bootstrap VM tests passed; JavaScript syntax, Ruff and mirror checks passed. | Includes invalid-input retry, key nondisclosure, non-Error rejection handling and the correct model environment variable. Real Wasm/browser execution is a remaining gate. |
| Jobs fixture synchronization | Both changed cases passed in 8.741 s against the frozen integrated binary; pinned Ruff lint/format passed. | The provider barrier and owner-lock probe eliminate two observation races; no production scheduler change or increased timeout. |
| Benchmark policy replacement | Language suite: 14 tests including 12 mutants. Runtime-policy suite: six tests including 17 mutants. Archived evidence: four tests. All passed. | Cases and exact arithmetic are described above; no equivalence claim with the retired general theorem. |
| Lean golden checking | Seven helper controls passed. ACP: 10 Lake jobs/three goldens; dictation: 17 jobs/three goldens; GUI: 15 jobs/five goldens. All passed. | Full packages built and actual exports matched exact committed path sets and bytes without modifying them. |
| Production C policy | 37 specification obligations, one helper obligation and 22 guarded non-overflow obligations passed; 6,054 GCC/Clang/UBSan/Lean vectors agreed. | The checked source is actual production C, with width/conversion assumptions encoded in the translator and specification. |
| Production C mutations | Passing control, then all 30 mutants rejected: 20 by Lean and 10 by the translator, at the expected stage. | No mutation can pass vacuously because the unchanged control is required first. |
| Standalone SMT | Three retained ACP catalog queries and six shell obligations returned unsatisfiable. | The removed ACP query's contradictory assumption is not counted as a meaningful retained guarantee. |
| Review and style | Cross-reviews and focused style checks completed; no unresolved blocking issue remained after review corrections. | Review caught and corrected the Wasm model variable, orphaned accessor documentation and terminal listener cleanup ownership. |

### Retained-proof input identity

Before publication, a read-only comparison matched the original proof-tested
source (local `865c37e7288843417971d04c622ac441c83ac0d2`), its integration
(`2f0506d6` / local `351a3e8e`) and the final code/test tree (`c05e2de8` /
local `3412ba39`). All 52 relevant tracked inputs had identical file sets,
modes, Git blobs and SHA-256 bytes. This includes the complete ACP, dictation,
GUI and C-policy proof packages, toolchain/package configuration, committed
goldens, shared checker and its controls, C checker/mutation driver and its
production C input closure. There were no changed proof inputs requiring an
expensive repeat.

The canonical 52-record manifest, sorted by path with records
`MODE BLOB_SHA1 FILE_SHA256 PATH`, had SHA-256
`d8c88828a65a202388ac064dd48f068dee43bc1a0a3951d9c46e9dc1d7a73265`.
The package trees provide compact reproducible identifiers:

| Package | Unchanged Git tree |
| --- | --- |
| `tests/formal/acp/` | `2caf20f8b7557df68f56dec37e30d64a7e8b6473` |
| `tests/formal/dictation/` | `2fce3072062ddf5fce09b940dc66b115241d1f58` |
| `gui/proofs/` | `0b550252dcdaeb0da77f892169dd796aba821f5a` |
| `tests/formal/python_code_mode/` | `9d59cff80b27a857870f9d113bb1a91374ff3bd0` |

The six inputs outside those package trees are
`tests/formal/check_lean_golden.py`, `tests/formal/test_lean_golden.py`,
`tests/formal/check_code_policy.py`, `src/core/code_policy.c`,
`src/core/code_policy.h` and `src/core/code_runtime.h`. Package manifests
use Lean 4.30.0 with their own modules and Std, and no external Lake packages.
This comparison carries the completed proof results to the final input tree;
it does not extend their documented modeling scope.

A second read-only comparison after M18/M21/M22 integration matched all 52
paths, modes and Git blobs against `c05e2de8`. It recomputed the identical
manifest hash above for follow-up code commit
`01bb08a9223690933aad212ed38f9de01e97b072`, tree
`3e6f45958d5c3818b8315972998a7082796aea4f`. The audit-only publication does
not change those inputs. This extends input identity through the follow-up
source without rerunning the expensive proofs or broadening their scope.

## Integrated gates and remaining coverage

The original workspace was reset during publication after the code commits had
been preserved on GitHub with identical trees. Detailed scratch logs and reports
were lost. Counts below are directly observed execution history reconstructed
from the completed review record, not claims that those log artifacts are still
available. The final aggregate integration run lacks a recoverable completion
summary and remains unverified.

| Gate | Result | Input and qualification |
| --- | --- | --- |
| Baseline full quality | Passed, exit 0. | Untouched audit baseline. |
| Original production full quality | Passed, exit 0. | `385a8475` / local `1071d26d`; all 143 C and 12 C++ translation-unit lanes completed, together with the configured formatting, lint and analyzer gates. Later M20 is a checked Python fixture; M18/M21/M22 have separate follow-up evidence below and require the final-head CI gates. |
| Original production GCC and Clang strict-warning lanes | Both passed, exit 0. | `385a8475`; `make warn-strict CC=gcc-14 CXX=g++-14` and `CC=clang-18 CXX=clang++-18`, using the pinned CPython cache. Subsequent follow-ups do not change C/C++ production inputs. |
| Full unit/sanitizer repeat | Passed, exit 0: 651 total, 649 passed, two platform skips, 54,069 assertions; no sanitizer errors. | Unchanged repeat on `efffe74a` / local `8f14b968`. This is not a claim that the first run was clean; see failures below. |
| Full Valgrind unit and CLI gate | Passed, exit 0: 651 total, 649 passed, two platform skips, 54,078 assertions; leak checks clean. | Same `efffe74a` source checkpoint. |
| ABI suite and manifests | 42 cases passed after two corrected-environment retries; one optional fault-library skip, zero unresolved case failures. All three manifest commands passed. | The initial ABI target exited 2 on two archive-path linker failures. Each unchanged affected case then passed with the same archives resolved absolutely. Tested core/library/schema inputs are unchanged from `8cb952f8` through final production. |
| Python SDK | Target passed, exit 0: 107 total, 106 passed, one skip; 47.316 s. Conformance: eight applicable passes, two explicit unsupported cases. | `TNY_TEST_BUNDLED_WHEEL` was unset, so optional installed-wheel coverage did not run. Permission capabilities are explicitly unsupported under ADR 0174. |
| TypeScript SDK | Target passed, exit 0: 60 total, 59 passed, one skip; 35.022 s. Conformance: eight applicable passes, two explicit unsupported cases. | `TNY_TEST_LEGACY_LIBTNY` was unset: the optional check loading earlier ABI 1.0–1.3 libraries with the current addon and preserving native usage defaults did not run. Permission cases follow ADR 0174. |
| Baseline full integration | Completed with exit 2: 104 programs, 101 successful, three failed programs containing four known failing cases; 78 unittest skip executions. | Failures: one CPython fixture environment mismatch, two jobs observation races and one instruction-improvement timeout. Explicit no-op notices are distinct from unittest skips. Focused resolutions are recorded below. |
| Final aggregate integration | **Not established as complete or passing.** The original session eventually reported exit 2, but its final output/coverage is unavailable after reset. | Run began on `efffe74a`. Last observed phase was jobs with 125 progress markers and no newly observed failure beyond the known CPython fixture mismatch. This cannot establish the remaining programs' results or a clean aggregate gate. |
| Paired CPython graph fixture retries | Baseline and final each passed one unchanged test, zero failures/skips: 7.940 s and 7.281 s. | Normal make/GCC 14/G++14 with inherited `CPYTHON_DIR`, `PY_RUNTIME_LDLIBS` and make overrides removed. The fixture uses fake Python inputs; the actual pinned runtime archive was unchanged. |
| Instruction-improvement repeat | Unchanged full module passed all 25 tests in 13.607 s at original budgets. | The baseline failure's specific cause remains unproven; this repeat does not rewrite the original outcome. |
| Baseline worktree integration module | All 33 tests passed in 76.845 s. | The last module of the untouched baseline aggregate run, included in its 104-program total; not a separate run against changed production code. |
| Retained proof gates | Completed and applicable to final inputs. | Exact-input comparison above closes this scope without rerunning unchanged expensive checks. |
| Real Wasm/browser, Rust GUI, Nix and macOS platform behavior | Not executed to completion locally; remain required coverage outside this Linux record. | Current project CI has Linux/macOS and Wasm checks. No Windows CI or AppKit/GTK behavioral validation is claimed. |

### Initial failures and what the repeats establish

The baseline unit test
`code_fatal_quota_does_not_run_finally_or_following_bytecode` initially did not
find the expected “memory limit exceeded” text. Its unchanged isolated repeat
and the complete 22-test code-runtime suite then passed. The original failed
assertion returned before freeing its output, producing a 64-byte LSan report.

The first integrated unit run failed `code_limits_are_sticky` when its
five-second deadline expired with `error: code: deadline exceeded`. That
assertion likewise returned before freeing output and reported 64 bytes.
The full unchanged unit repeat subsequently passed with the counts above.
These passing repeats provide useful evidence but do not prove a deterministic
cause for either initial failure.

The first ABI invocation failed to link two copied-source fixtures because a
relative embedded-CPython archive path did not resolve in their disposable
build directories. After correcting only the scratch archive-path setup, the
two unchanged cases passed individually in 51.847 s and 79.732 s. The initial
target failure is part of this record; the later 42-pass disposition is not
presented as one clean initial invocation.

Both baseline and integrated aggregate runs encountered
`CppBuild.test_cpython_runtime_graph` with an inherited absolute
`CPYTHON_DIR`, while the fixture expects the normal relative default.
The paired unchanged clean-environment retries passed. They resolve that
specific fixture-environment failure without altering the pinned CPython
archive or weakening the fixture.

The two baseline jobs failures came from assumptions about observations, not a
changed scheduler. Holding a response for 0.2 seconds could not guarantee that a
0.2-second subprocess polling loop saw the running state. A terminal projection
also intentionally precedes final reservation/slot cleanup and owner-lock
release. M20 now coordinates on the provider response and the real owner lock,
retains all privacy assertions, and adds nonempty-process/final-success checks
to prevent vacuous passing. Both changed cases passed at the original budgets.

The baseline instruction-improvement timeout remains recorded with its cause
unproven. All 25 unchanged module tests passed on repeat at their original
two-second per-command budgets. A passing repeat is not evidence for deleting
the check or raising its timeout.

## Publication follow-ups and GitHub checks

The first published head, `c05e2de8`, started fresh
[native CI](https://github.com/thehumanworks/tny/actions/runs/39175532143),
[SDK CI](https://github.com/thehumanworks/tny/actions/runs/39175532137), and
[desktop CI](https://github.com/thehumanworks/tny/actions/runs/39175532136).
At the 2026-10-18 23:35 UTC status snapshot, nine native jobs had passed: quality, Valgrind,
fuzzing, ThreadSanitizer, both musl builds, Wasm including the real browser
smoke, retained Lean gates, and Linux ARM64's unit/ownership/ABI/packaging lane.
The Linux x86-64 aggregate integration job was still running. Eight SDK jobs
had passed; the two macOS SDK jobs were still running. The
[source snapshot](https://github.com/thehumanworks/tny/actions/runs/39175532132)
and [offline language verification](https://github.com/thehumanworks/tny/actions/runs/39175532156)
workflows also passed. These are observed initial-head results, not a claim
that a still-running workflow had completed.

The desktop run's Ubuntu job could not find `fontconfig.pc`; its macOS job
passed 68 of 70 tests and failed the two index fixtures. The GUI Lean job
passed. M21 corrects the missing build prerequisite and the demonstrated
temporary-root mismatch, keeping all production index policy unchanged.

Native macOS CI passed its unit and code-execution checks, then failed a
shell-workflow assertion that expected the dependency-size diagnostic. Its
unexpected stderr contents were not printed. Investigation reproduced a
preexisting production result-loss race on both Bash and Zsh, including loss
of that exact diagnostic. M22 fixes the demonstrated race. Its relationship
to the historical macOS failure is a supported inference, not a directly
observed causal trace; the macOS follow-up CI remains necessary.

| Follow-up | Published code revision | Completed focused evidence |
| --- | --- | --- |
| M21 desktop setup | `1a06909fe467015f7b93e3e831de9af048f9cb33` | Actual-source physical-root control: two passes; unchanged symlink-root control: two failures at the macOS assertions; corrected fixtures with symlink root: two passes. Rust 1.75 Linux compiler/format checks, pinned actionlint 1.7.12 and the Fontconfig 2.15.0 metadata probe passed. Full Slint/Cargo and macOS execution are CI scopes. |
| M18 conformance contract | `37629761be1027ff46af3593f088e7c0a245cfa3` | All 18 prior predicates remain identical. Six new tests cover 24 malformed variants in normal, `-O` and `-OO` Python: 72 required rejections plus three valid controls. Full `make test-conformance-contract`: nine tests passed in 16.617 s; Ruff 0.16.6 lint/format passed. |
| M22 shell reaping | `01bb08a9223690933aad212ed38f9de01e97b072` | Both unchanged-source controls fail on the result-loss race. Final full `make test-shell-workflows` passed in Bash 5.2.21 and Zsh 5.9, including success, specific byte-limit failure and true crash, plus all existing cancellation/privacy/negative controls. ShellCheck 0.11.0, shfmt 3.14.0 and syntax checks passed. Independent review required immediate same-pass PID-record cleanup and exactly-once reaping. |

Each follow-up Git tree matched the locally reviewed index exactly before
publication. No existing test gate was removed or given a larger timeout.
The final reviewed head's results belong to the
[PR checks](https://github.com/thehumanworks/tny/pull/201/checks), and its
current verification summary is maintained in the pull-request description.
The interrupted local aggregate remains an unverified historical run;
later CI results do not rewrite it as a local success.

## Recommended next sequence

These follow-on candidates have explicit prerequisites and scope or behavioral
risks. Keep their verification gates attached to each change. The most useful
next sequence is:

1. **R17 — specify and harden URL parsing.**
   Define accepted endpoint grammar and build boundary fixtures before changing
   permissive parsing behavior.
2. **M06 and M17 — clarify build ownership and reduce repeated CPython work.**
   Map Make/Nix input closure and cache identity first, then measure clean and
   incremental cost across supported targets.
3. **R13, M11 and R14 — split the largest runtime ownership surfaces.**
   Begin with pure jobs validation/formatting and explicit library/provider
   boundaries; keep crash, cancellation, OOM, ABI and replay gates attached to
   each extraction.
4. **M07 and M19, coordinated with a Pages migration if desired — organize
   evidence and completed plans.** Establish references, hashes and history
   before archival. Site mirror removal requires the separate deployment
   contract and browser verification described in M09/S27.

The completed changes have direct and integrated evidence, with initial
failures and coverage gaps stated above. The local record alone does not
establish zero regressions, a passing final aggregate integration run, or full
validation on every supported platform. Use the separately linked CI results
for subsequent automated coverage of the reviewed head.
