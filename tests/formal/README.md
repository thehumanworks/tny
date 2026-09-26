# Formal contracts

Two formalisms live here. `dictation/` is a Lean 4 project (below); the rest
is SMT-LIB 2 with Z3.

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

`acp_catalog.smt2` models already-parsed select/legacy options. It does not
model JSON parsing, malformed options, credentials or SDK availability. The
linked concrete test is `test_acp_catalog_provenance_newer_and_stale_adapter`.

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
