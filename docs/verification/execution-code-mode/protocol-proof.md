# Execution protocol: source-linked machine checking

Run `python3 tests/formal/check_execution_protocol.py` from the repository.
It requires a native Clang C compiler and Z3; a missing tool, unsupported AST,
compiler diagnostic, solver `unknown`, timeout, or changed source fails the check.
`TNY_FORMAL_CLANG` and `TNY_FORMAL_Z3` select explicit tool executables.

## What is checked

The checker translates **the actual `tny_exec_protocol_admit` C function** in
`src/core/execution_protocol.c`, using Clang's typed AST. This is not a separately
maintained transcription of the predicate. Only side-effect-free integer/Boolean
expressions, casts, `if`, and `return` are supported. New syntax fails closed and
requires review of the verifier. Integer conversions and signed/unsigned
comparisons use fixed-width SMT bitvectors, rather than unbounded mathematical
integers. Compiled assertions establish the required native type widths.

Z3 searches for counterexamples over every 32-bit phase and message-kind value,
and every 64-bit request and expected identifier value: all 192 input bits.
Seven obligations establish:

- Exact equivalence with an independent allowed-case specification.
- Zero identifiers cannot authorize dispatch.
- Terminal and invalid phases cannot authorize dispatch.
- Execution admission requires the initial phase and identifiers.
- Nested replies require the expected result kind and exact correlation.
- Callback requests cannot borrow the outer result's identifier.
- Outer results cannot borrow a callback's identifier.

All negated obligations must be `unsat`. A valid initial request must additionally
be `sat`, so rejection of every request cannot pass vacuously. The verifier also
compiles the exact production function text against its real header, obtains enum
values from that executable, and compares 9,600 representative input combinations
against an independent C specification. These finite checks are a cross-check on
the translation, **not** the exhaustive proof. Each successful run prints the
verified predicate's SHA-256.

The optional `--source` argument exists for mutation checks. Normal verification
must omit it, so the repository's production implementation is checked.

## Limits of the claim

This is source-linked, translation-based SMT verification of one **pure admission
predicate**. The trusted base includes this small AST translator, Clang's AST and
integer typing, Z3, the compiler used for the representative cases, and the stated
C type-width assumptions. The translator and solver are not themselves formally
verified. The check does not prove the entire C program or prove that callers
maintain a valid protocol state or use this predicate at every necessary boundary.

It does not establish JSON parser correctness, frame completeness, descriptor
ownership, process cleanup, deadlines, exactly-once external effects, or resistance
to a compromised same-user host or interpreter. Those require concrete integration
and failure-injection tests. In particular, **the actual filesystem and permission
policy are not formally verified by this predicate**: current tests must establish
that nested operations still pass the server's ordinary permission checks and
cannot widen the trusted context. A transport failure after an effect is not a
transaction rollback or a safe reason to replay the script.
