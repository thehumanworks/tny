# Python code-mode runtime selection — preregistered protocol

Date: 2026-09-27. Written and committed **before** any held-out Luna trial.
The user has decided to migrate code mode from Lua to Python and accepts
CPython's size if no lighter Python runtime preserves code-generation
efficiency. This protocol selects *which* Python implementation, not whether
to use Python. PR #197's published evidence (`../code-mode-language/`) is
preserved unchanged.

## Candidates and pre-trial evidence

Primary-source eliminations without a build (PyPy, RustPython, GraalPy,
Starlark, compilers, microcontroller subsets) are recorded in the scratchpad.
Built and measured, same host, GCC 16.2.1, the preserved benchmark host
(`tests/bench/code_mode/host.c`) and its `-Os -flto=auto` section-GC flags:

| Executor | Stripped bytes | Empty cell (init+eval+destroy) median | Semantic cases matching CPython (127) | Frozen PR #197 corpus |
|---|---:|---:|---:|---:|
| empty host | 96,568 | 0.000 ms | — | 0/36 |
| PocketPy 2.2.0 | 630,504 | 2.393 ms | 67 | 27/36 |
| MicroPython 1.29.0 | 371,200 | 0.223 ms | 88 | 33/36 |
| Monty 1.0.0 (Rust, fat LTO, `opt-level=s`) | 6,165,632 | 0.789 ms | 103 | 36/36 |
| CPython 3.14.7 static (no LTO yet, encodings frozen, no stdlib dir) | 4,668,312 | 13.793 ms | 127 (reference) | size/startup probe only |
| CPython 3.14.7 stock shared (PR #197 adapter) | 117,568 + libpython | — | 127 | 36/36 (reproduced) |

Corpus replays run the 36 published programs unmodified against all three
hidden variants in the preserved bubblewrap envelope and scorer
(`replay.py`). Semantic cases (`semantics.py`) are diagnostics, not the
selection criterion.

- **PocketPy**: genuine syntax failures on generator expressions used by the
  published programs (`sorted(x for …)`, `all(… for …)`): eliminated on
  actual task failures.
- **MicroPython**: 33/36 are real passes under the published contract
  (typed JSON values; serialized key order is not part of it); the three
  Unicode failures are its json module's missing `ensure_ascii`, which a
  native facade could fix. Separately, its dicts are not insertion-ordered
  (diagnostic `dict_order`), so programs whose required output order comes
  from dict insertion, or that rewrite existing JSON and must keep its key
  order, behave differently from CPython; it also lacks stable
  `reverse=True` sorting and stepped Unicode slicing. Eliminated on those
  insertion-order semantics, independent of the fixable `ensure_ascii` gap.
- **Monty**: 36/36 corpus. Documented subset: no `yield`, `del`, class
  inheritance/custom exceptions or `match`; eager generator expressions;
  `True + 1` raises; no `str.lower` as an unbound key; no `callable`,
  `issubclass`, `ascii`. It remains a candidate for the paired trial.

## Held-out paired generation trial

Twelve new task families (`heldout.py`), authored after and different from
the published twelve, over the same five virtual capabilities. Three hidden
deterministic variants each; six families additionally require exact object
key order. Handwritten controls pass 108/108 variant executions on all arms
before inference (`trials.py --controls`).

Arms (only the language-help paragraph and executor differ):

1. `cpython` — proposed production wording; stock CPython 3.14.7 executor
   with the proposed production policy (all builtins except import/code-
   loading/interactive ones; `print(sep=, end=)`; `error: Type: msg (line N)`).
2. `cpython_pr197` — the exact PR #197 Python paragraph and its preserved
   narrow-builtins executor. Pairs the production wording change itself.
3. `monty` — the production wording plus an honest statement of Monty's
   subset limits; Monty 1.0.0 executor.

Generation reuses the preserved `run.generate`: normal Codex CLI ChatGPT
login, `gpt-6-luna`, low effort, code-only schema, no generation-time tools,
three workers, arm order rotated within task/repetition blocks. 12 families ×
3 repetitions × 3 arms = 108 first generations. A failure gets exactly one
fresh-session repair with its code and observed failures, never expected
answers. Every first attempt, failure and usage receipt is retained; no
sample is rerun or selected. Invalid generations stay in denominators.

## Decision rule (fixed before inference)

CPython is the default. A lighter runtime replaces it only if
`policy.select_lighter` holds: complete 36/36 frozen corpus; complete trial
cohort; no observed first-pass or final-success regression versus the
`cpython` arm; output tokens per solved task no higher than the `cpython`
arm's (integer cross-multiplication); the production semantics it must
preserve (insertion-ordered objects, signed 64-bit and larger JSON integers,
null/false/zero/empty shapes, Unicode scalar slicing) verified; **and**
smaller stripped bytes than the static CPython probe under equal flags.
Reported alongside, not as extra gates: task-clustered paired bootstrap
intervals (10,000 draws, seed 20260927), repairs, uncached input tokens,
wall time and failure categories. Twelve families cannot establish general
language or runtime superiority.

Monty's probe is already larger than the static CPython probe, so under this
rule Monty cannot be selected whatever the trial shows; the trial is still
run as preregistered to measure the generation cost of subset disclosure and
to provide held-out evidence for the production CPython wording
(`cpython` vs `cpython_pr197`). A production-wording regression would be
reported and addressed before release, not hidden.

## Formal scope

Lean checks definitions generated from `policy.py`'s actual AST (same
whitelist translator approach as PR #197): acceptance and selection gates,
non-vacuity, and negative mutations that must fail. This does not certify
the stochastic trial, the interpreters, or OS containment.

## Notes recorded after launch (17:30, no protocol change)

- Supervisor review flagged `config_merge` wording: "when both values are
  objects, merge" versus "an empty object replaces". The oracle (`_merge`)
  merges whenever both are objects. A check of all three fixtures finds no
  override that is an empty object over a non-empty default object, so the
  ambiguity cannot change any score in this cohort; wording is unchanged to
  preserve the preregistered prompts and manifest hashes.
- These families are held out from PR #197 but were authored **after** the
  candidate semantic diagnostics were known. Six of twelve require explicit
  object key order. They are targeted compatibility tasks, not an unbiased
  sample of coding workloads; the generic frozen-corpus results stay primary.
- The `cpython` arm's executor exposes every builtin except the import,
  code-loading and interactive ones (`cpython_prod.c`), matching its prompt.
  The `cpython_pr197` arm deliberately keeps PR #197's narrow whitelist.
