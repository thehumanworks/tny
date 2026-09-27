# Python code-mode proofs (Lean 4.12.0)

Source-linked Lean proofs for the Python code-mode migration. Nothing here
is a hand-maintained copy of production logic: each checker reads the
production source, translates it through a fail-closed whitelist, and
prepends the generated definitions to the proof file before running `lean`.
Scope, evidence and limitations:
[`docs/verification/python-code-mode/proofs.md`](../../../docs/verification/python-code-mode/proofs.md).

| File | Role |
|---|---|
| `../check_code_policy.py` | Clang typed AST of `src/core/code_policy.c` → Lean `BitVec`/`Bool` definitions, generated no-wrap obligations, compiled gcc+clang UBSan vector cross-check |
| `CodePolicy.lean` | Independent contract and theorems for the eight C gates |
| `check_runtime_selection.py` | Python AST of `tests/bench/python_runtime/policy.py` (read-only) → Lean `Int`/`Bool`; replays the recorded held-out decision |
| `RuntimeSelection.lean` | Theorems for the preregistered `select_lighter` gate |
| `test_mutations.py` | Source mutants that must fail at the expected stage (Lean vs translator) |
| `lean-toolchain` | Exact pin; both checkers refuse any other `lean --version` |
| `lakefile.toml` | Dependency-free, so `leanprover/lean-action` can install the pin |

```sh
LEAN=/path/to/lean-4.12.0 TNY_FORMAL_CLANG=clang TNY_FORMAL_GCC=gcc \
  python3 tests/formal/check_code_policy.py
LEAN=... python3 tests/formal/python_code_mode/check_runtime_selection.py
LEAN=... TNY_FORMAL_CLANG=clang TNY_FORMAL_GCC=gcc \
  python3 tests/formal/python_code_mode/test_mutations.py
```

Exit status: 0 proved; 1 Lean rejected the proofs (or a tool failed);
3 the source left the translator whitelist. `--emit FILE` writes the exact
Lean file that was checked. Proof files may not contain `sorry`, `admit`,
`axiom`, `native_decide`, `bv_decide`, `implemented_by`, `extern`, `unsafe`,
`opaque`, `partial` or `#exit`, nor declare or notate anything named like a
generated definition (`tny_code_*`, `select_lighter`), which could shadow it
under `open`. `#print axioms` output for every declared theorem and
generated obligation must be within `propext`, `Classical.choice` and
`Quot.sound`.
