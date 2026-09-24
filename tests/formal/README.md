# Formal contracts

Language: SMT-LIB 2, solver: Z3. Run `make verify-formal` (also in `make test`).
Each `.smt2` file contains `check-sat` queries for **negated** invariants.
The checker requires exactly one `unsat` per query, and fails closed on
`unknown`, `sat`, missing Z3, parser diagnostics or timeouts. Use fresh files
for independent boundaries. Explain abstractions and tie each contract to a
fixture test of the actual implementation; this is not a C correctness proof.

`acp_catalog.smt2` models already-parsed select/legacy options. It does not
model JSON parsing, malformed options, credentials or SDK availability. The
linked concrete test is `test_acp_catalog_provenance_newer_and_stale_adapter`.
