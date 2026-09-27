# Python code mode — formal verification scope and evidence

Date: 2026-09-27. Lean 4.12.0 (`dc2533473114`), Clang 22.1.8, GCC 16.2.1,
CPython 3.14.7, x86_64 Linux (LP64). Checkers, proofs and mutants live in
[`tests/formal/check_code_policy.py`](../../../tests/formal/check_code_policy.py)
and [`tests/formal/python_code_mode/`](../../../tests/formal/python_code_mode/README.md).

## What is proved

### C code-cell gates (`src/core/code_policy.c`)

`check_code_policy.py` asks Clang for the typed JSON AST of the whole
translation unit, requires that it defines exactly the eight gates, and
translates each body into a Lean definition. Types are the C types: `_Bool`
→ `Bool`, `int` → `BitVec 32` (signed comparisons), `int64_t`/`uint64_t` →
`BitVec 64` (signed/unsigned comparisons). Clang's implicit conversions are
translated (`signExtend`/`zeroExtend`), and unsigned `+ - *` are wrapping
`BitVec` operations, so the model has C's modulo-2^N arithmetic rather than
natural numbers. Loops, calls, locals, pointers, globals, enums, division,
shifts, narrowing, unsigned→signed conversion and signed arithmetic (which
could be undefined behavior) are rejected before Lean runs.

Short-circuit evaluation is checked, not assumed: for every unsigned
arithmetic node the translator emits a Lean obligation that it cannot wrap
under the exact `&&` / `||` / `?:` / `if` path condition on which C
evaluates it. For example, `limit - used - header` in the memory gate is
only proved exact under `used <= limit && header <= limit - used`; deleting
that guard makes the generated obligation false. Because every accepted
expression is total and side-effect free, evaluating both operands (as the
Lean terms do) gives the same value as C's short-circuit order.

`CodePolicy.lean` restates the intended limits as plain numbers, independent
of the C source, and proves for **all** inputs of the stated widths:

| Gate | Theorems |
|---|---|
| timeout (`int64_t`) | admits exactly `1 ≤ t ≤ 30000` as a signed value; 0, 30001, −1, −30000, INT64_MIN/MAX and every `t ≤ 0` rejected |
| source | exactly `bytes ≤ 262144`; 262145 and UINT64_MAX rejected |
| nested call | exact spec; **no call once 64 are done**; each of the first 64 is admissible; name 1..256 bytes, not flagged recursive (`run_code`), arguments ≤ 256 KiB and a JSON object |
| tool result | exactly `≤ 8 MiB − 16`; result + 16-byte header fits one frame |
| printed output | exactly `used + add ≤ 65536` over ℕ; the C sum `used + add` is exact when admitted; any pair whose sum wraps 2^64 is rejected |
| heap accounting | exactly `used + header + request ≤ limit` over ℕ; the admitted C sum is exact; wrap attempts rejected |
| parent frame | exact spec; admitted ⇒ phase RUNNING, so **FINISHED and FAILED admit nothing**; only CALL/DONE; never empty; no CALL frame after 64 calls; payload boundaries 262402/262403 (CALL) and 66049/66050 (DONE) |
| cross-gate | every child-admitted call's frame (`'C' name '\n' args`) is parent-admitted; every admitted output plus a ≤512-byte error line fits a DONE frame |
| JSON kind | first-match spec over all 128 flag combinations; **None first** (`= NULL ↔ is_none`); **bool before int** (a bool is never INT; `false` never becomes 0); INT and DEFAULT exact; result in 0..7 |
| non-vacuity | every gate admits a witness; every JSON kind is reachable |

Result on the snapshot below: **36 specification theorems** (+1 helper
lemma) and **17 generated no-wrap obligations** proved, 31 s wall time.
`#print axioms` for all 54: 17 use `propext, Classical.choice, Quot.sound`,
19 `propext, Quot.sound`, 6 `propext`, 12 none. No `sorry`, `native_decide`,
`bv_decide` or new axioms (the checker rejects them textually).

### Runtime selection (`tests/bench/python_runtime/policy.py`, read-only)

`check_runtime_selection.py` parses the preregistered gate with Python's
`ast`, requires the exact preregistered signature, and translates it to Lean
over `Int` (Python's unbounded `int`) and `Bool`. Only single comparisons,
`+ - *`, non-negative int literals and `and`/`or`/`not` over bool operands
are accepted. `RuntimeSelection.lean` proves: the exact decision rule;
incomplete trials, a not fully passing or empty corpus, first-pass or final
regressions, zero solved tasks, a token-per-solved regression and missing
production semantics each reject; the cross-multiplied token criterion is
invariant under scaling the lighter arm's tokens and solved count by any
positive factor (it compares ratios); **a lighter runtime not strictly
smaller than static CPython is never selected** (protocol.md's size rule);
and a selectable input exists. Result: **13 theorems** (+1 helper) proved;
axioms within `propext, Classical.choice, Quot.sound`; 6 s.

The recorded held-out decision in `data/heldout-analysis.json` is replayed:
the imported Python function and the Lean kernel both return `false`, and
Lean derives that rejection from `size_gate` (6,165,632 ≥ 4,668,312 bytes)
and from `semantics_required` independently of the trial outcome.

## Cross-checks (not proof)

- The exact production `code_policy.c` is compiled with GCC and Clang under
  UBSan (`-fno-sanitize-recover=all`; Clang also with
  `unsigned-integer-overflow` and `implicit-conversion`) and run on
  boundary vectors derived from every constant each parameter is compared
  with; both compilers must agree, and Lean's kernel must compute the same
  result from the generated definitions (`decide`, no `native_decide`).
- The Python gate is evaluated on the recorded inputs plus seeded vectors;
  Python and the Lean kernel must agree.
- Result: 4550 C vectors agree (admitted/nonzero per gate: timeout 4/9,
  source 4/6, call 48/1008, result 4/6, output 22/72, memory 16/81,
  frame 45/3240, JSON kind 64/128 non-null); 372 selection rows agree
  (33 selected), including the recorded decision.
- A compiler probe asserts the width and signedness of every C type the
  translator maps, and two's complement, for both compilers.

## Negative mutations

`test_mutations.py` copies the production sources, applies each mutant, and
requires the failure at the stated stage. An unmodified copy must pass first.

Result: **38/38 rejected at the expected stage** (≈2.5 min).

Rejected by Lean theorem checking (translator accepts, a theorem or a
generated obligation fails):

- C: call budget `<`→`<=`; recursion flag ignored; object check dropped;
  empty name admitted; timeout admits 0; output guard `used <= 65536`
  dropped; heap guard `used <= limit` dropped; heap check rewritten as
  `request + header <= limit - used` (wraps); result bound rewritten as
  `r + 1 <= limit + 1` (wraps at UINT64_MAX); frame phase `== RUNNING`→
  `!= FAILED`; CALL-frame budget dropped; DONE bound off by one; empty
  payload admitted; int tested before bool; None not tested first;
  `TNY_CODE_TOOL_CALLS` 64→65; `TNY_CODE_SOURCE_BYTES` 256→257 KiB.
- Python: size gate dropped; size `<`→`<=`; semantics check dropped;
  completeness dropped; `solved_cpython > 0` dropped; token comparison
  reversed; first-pass comparison reversed.

Rejected by the translator (fail closed, Lean never runs):

- C: `for` loop; function call; unsigned `/`; `>>`; signed `+` on a
  parameter; narrowing cast; local variable; file-scope mutable global;
  enum constant; an extra, unproven exported gate.
- Python: non-bool `and` operand; chained comparison; helper call; `//`.

## Inputs (snapshot) and commands

The C inputs are the runtime author's uncommitted snapshot present in this
worktree when the proofs ran; the checker always reads `src/` and prints
fresh hashes, so rerun it on the integrated commit.

| Input | SHA-256 |
|---|---|
| `src/core/code_policy.c` | `318ad973e5540697f9f88eede07b17447058e005eec69c5512b14e65f73f1e31` |
| `src/core/code_policy.h` | `75e6eeb4387e9b6ad8584c5f8d06b9b904cc5dd469f04f79d71f064a3d043e0e` |
| `src/core/code_runtime.h` | `0a5575617fca8c80f3f334bd32c001fe3cd78f15aba80efabcd665310e7b76ea` |
| `tests/bench/python_runtime/policy.py` | `04819b8fe259e9a9d45b8eb8e229d03cc4f331c6d34afb595335cf4ce505bae5` |

```sh
LEAN=<lean 4.12.0> TNY_FORMAL_CLANG=clang TNY_FORMAL_GCC=gcc python3 tests/formal/check_code_policy.py
LEAN=<lean 4.12.0> python3 tests/formal/python_code_mode/check_runtime_selection.py
LEAN=<lean 4.12.0> TNY_FORMAL_CLANG=clang TNY_FORMAL_GCC=gcc python3 tests/formal/python_code_mode/test_mutations.py
```

## Trusted base and limitations

- Trusted: Clang's parser and type checker (including where it inserts
  conversions), the two translators, CPython's `ast`, the Lean 4.12.0 kernel
  and the standard axioms `propext`, `Classical.choice`, `Quot.sound`; the
  LP64 ABI asserted by the probe (an LLP64 or non-two's-complement target
  fails the probe instead of being modeled).
- The proofs cover the eight predicates, not their callers: that the parent
  passes the true phase, frame type, payload length and call count, that the
  child computes the JSON flags with the right `isinstance` tests, the
  interpreter, frame encoding, OS confinement and the deadline are outside
  this proof. Compiler code generation is only cross-checked by vectors.
- The limits in `CodePolicy.lean` are restated by hand from the contract;
  the proof shows code and restated contract agree, not that the contract
  values are the right product choice.
- `policy.py`'s annotations are not enforced by Python. The translation is
  sound because every `and` operand is a comparison or a bool-annotated
  parameter, so `and` returns a bool; `analyze_trials.py` passes a
  `store_true` flag and a conjunction of comparisons. A caller passing
  non-bool values is outside the proof.
- The selection proof is about the decision rule. Trial outcomes, token
  counts and pass rates are stochastic measurements; nothing here is a
  theorem about model or runtime quality.
- Both checkers refuse any Lean other than the `lean-toolchain` pin. The
  Nix sandbox's `lean4` is 4.30.0 and has not been run against these proofs.
