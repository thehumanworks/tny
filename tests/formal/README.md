# Formal contracts

Two formalisms live here: Lean 4 for production code-cell gates, dictation and
ACP contracts, and Z3 for source-linked execution admission.

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

## Z3: production execution admission

`make verify-execution-protocol` translates the production admission predicate
from Clang's typed AST, checks it against an independent allowed-case contract
with Z3, and cross-checks a compiled copy on representative values.
`make verify-formal` retains this check and the golden-exporter negative controls.
Unsupported source syntax or missing tools fails the check, never skips it.
This proves the pure predicate, not its callers or the surrounding RPC lifecycle.

### Retired detached models (2026-10-19)

The shell-mode and ACP-catalog SMT models were removed: they restated their own
transition equations or selector definitions, without extracting production code
or comparing model output to it. Their green results could not detect a C
regression. A fail-closed solver launcher did not repair that missing connection.

Retained production coverage:
- `tests/test_tui.c`: child execution, merged streams, EOF responsiveness,
  built-in/blank prompt preservation and disclosure-budget refusal.
- `tests/integration/test_tui.py::test_shell_disclosure_reaches_provider`: actual
  outgoing request contains command, output, cwd and exit status, and a second
  prompt does not repeat the disclosure in its new user message.
- `tests/integration/test_acp_client.py::test_acp_catalog_provenance_newer_and_stale_adapter`:
  actual subprocess output preserves advertised model IDs and names.

These tests are bounded evidence, not all-input proofs. In particular the unit
shell test clears the disclosure buffer itself; it must not be cited as proof
that production prompt submission consumes it exactly once.

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
