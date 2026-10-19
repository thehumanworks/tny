# Formal contracts

Two formalisms live here: Lean 4 for production code-cell gates, dictation and
ACP contracts, and SMT-LIB 2 with Z3 for execution admission and shell/catalog
abstractions. GUI state-machine proofs live in `gui/proofs/`.

Benchmark-only decision proofs were replaced with direct offline tests by
[ADR 0188](../../docs/adr/0188-focused-verification-maintenance.md). The original
preregistered policies and measurement evidence are unchanged. The tests are
finite checks, not replacements for the retained all-input production proofs.

## Checking exported fixtures

`check_lean_golden.py PROJECT` builds the complete Lean package, exports into
a temporary directory and compares the exact file set and bytes against
`PROJECT/golden/`. It fails for changed, missing or extra files, build/export
failure and an empty committed fixture set. Verification never regenerates
tracked fixtures in place. `test_lean_golden.py` checks these failure modes with
a local fake exporter, without requiring Lean. To deliberately update fixtures
after a specification change, run `lake exe export golden` in the package and
review that diff with the proof and implementation changes.

## Lean 4: production code-cell gates (`python_code_mode/`)

[`check_code_policy.py`](check_code_policy.py) translates the actual typed Clang
AST of `src/core/code_policy.c` to width-exact bit vectors and generates
short-circuit overflow obligations. The independent contract covers nine gates:
timeout/source/call/result bounds, output admission/capture, UTF-8 lead widths,
heap accounting and parent frame admission. `make verify-code-policy` checks the
proofs, GCC/Clang UBSan vectors and negative C mutations. See the
[current checker guide](python_code_mode/README.md). This does not prove callers,
the interpreter, OS process behavior or compiler code generation.

## Lean 4: ACP admission and lifecycle (`acp/`)

[ACP proofs](acp/README.md) implement the stable minimum-version rule and model
fresh handshake authority, tools-only sessions, capability-gated resume and
reconnect rejection (ADR 0177). `make verify-acp-proofs` builds with the pinned
Lean toolchain and checks golden-table drift; CI runs it in `lean-proofs`.
`make test` replays `acp/golden/versions.tsv` against production C and
`acp/golden/transitions.tsv` through the subprocess ACP fixture, without Lean.
The general theorems prove the specification, not the C code or upstream adapter.

## SMT-LIB 2 (Z3)

Language: SMT-LIB 2, solver: Z3. Run `make verify-formal` (also in `make test`).
Each `.smt2` file contains `check-sat` queries for **negated** invariants.
The checker requires exactly one `unsat` per query, and fails closed on
`unknown`, `sat`, missing Z3, parser diagnostics or timeouts. Use fresh files
for independent boundaries. Explain abstractions and tie each contract to a
fixture test of the actual implementation; this is not a C correctness proof.

`shell_mode.smt2` models local shell start, bounded disclosure and prompt
delivery guards. Linked concrete fixtures are `shell_mode_streams_and_discloses_only_once`
and `shell_mode_denies_unrecordable_command` in `tests/test_tui.c`. The
`test_shell_disclosure_reaches_provider` PTY integration test checks the wire
request. It does not
model the OS shell or prove the C implementation.

`acp_catalog.smt2` models the provenance of already-parsed select/legacy catalog
**listings**. It does not decide whether a requested model is admitted: ADR 0181
requires unlisted IDs to reach the adapter, as checked by `acp/Model.lean` and
`test_model_selection_matches_lean`. It does not model JSON parsing, malformed
options, credentials or SDK availability. Its linked concrete test is
`test_acp_catalog_provenance_newer_and_stale_adapter`.

## Lean 4: dictation normalization (`dictation/`)

`dictation/` specifies and proves transcript normalization (ADR 0175) in Lean
4.30 with no Mathlib: correction application and its soundness
(`Dictation/Apply.lean`), text classes, the number grammar and a Levenshtein
row DP proven equal to its recursive specification (`Text.lean`), the
project-wins dictionary merge (`Dictionary.lean`), the verifier with fail-open
delivery (`Verify.lean`) and the normalization lifecycle invariants
(`Lifecycle.lean`). No `sorry` or axioms. `lake exe export golden` writes
`golden/lifecycle.tsv`, `golden/dictionaries.tsv` and `golden/verify.tsv`.

`make test` (via `tests/test_dictation.c`) replays those committed tables
against `tny_norm_step`, `tny_dictionary_parse`, `tny_norm_proposal_parse` and
`tny_norm_verify`, so it needs no Lean toolchain. `make verify-dictation-proofs`
(and the CI `lean-proofs` job) rebuilds the proofs and fails if regenerated
tables differ from the committed ones. Install the toolchain with elan; the
pin is `dictation/lean-toolchain`. Like the SMT contracts, this proves the
specification and ties the C implementation to it on the exported inputs; it
does not prove the C code, the transport or the model.
