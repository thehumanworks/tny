# Python code-mode proofs (Lean 4.30.0)

Source-linked Lean proofs for the production Python code-cell gates. The
checker reads the production C source, translates it through a fail-closed
whitelist, and prepends the generated definitions to the independent contract
and theorems before running `lean`.
Scope, evidence and limitations:
[`docs/verification/python-code-mode/proofs.md`](../../../docs/verification/python-code-mode/proofs.md).

| File | Role |
|---|---|
| `../check_code_policy.py` | Clang typed AST of `src/core/code_policy.c` → Lean `BitVec`/`Bool` definitions, generated no-wrap obligations, compiled gcc+clang UBSan vector cross-check |
| `CodePolicy.lean` | Independent contract and theorems for the nine C gates, including output capture and UTF-8 lead widths |
| `test_mutations.py` | Source mutants that must fail at the expected stage (Lean vs translator) |
| `lean-toolchain` | Exact pin; the checker refuses any other `lean --version` |
| `lakefile.toml` | Dependency-free, so `leanprover/lean-action` can install the pin |

```sh
LEAN=/path/to/lean-4.30.0 TNY_FORMAL_CLANG=clang TNY_FORMAL_GCC=gcc \
  python3 tests/formal/check_code_policy.py
LEAN=... TNY_FORMAL_CLANG=clang TNY_FORMAL_GCC=gcc \
  python3 tests/formal/python_code_mode/test_mutations.py
```

Exit status: 0 proved; 1 Lean rejected the proofs (or a tool failed);
3 the source left the translator whitelist. `--emit FILE` writes the exact
Lean file that was checked. Proof files may not contain `sorry`, `admit`,
`axiom`, `native_decide`, `bv_decide`, `implemented_by`, `extern`, `unsafe`,
`opaque`, `partial` or `#exit`, nor declare or notate anything named like a
generated definition (`tny_code_*`), which could shadow it
under `open`. `#print axioms` output for every declared theorem and
generated obligation must be within `propext`, `Classical.choice` and
`Quot.sound`.

The benchmark-only Python AST translator and `RuntimeSelection.lean` were
retired by [ADR 0188](../../../docs/adr/0188-focused-verification-maintenance.md).
`make test-code-mode-language` now checks the actual benchmark policy with
finite decision tables, exact-ratio oracles, archived input replay and mutations.
This does not replace or weaken the retained production C proof.
