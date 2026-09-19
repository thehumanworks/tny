# ADR 0151: Actionable exact-edit failures

Date: 2026-09-19. Status: accepted.

Implements one narrow agent-first optimization under
[ADR 0150](0150-agent-first-harness-and-measured-footprint.md).

## Problem

A local `edit_file` mismatch returned only `old_string not found`. The shared
exact-match editor had already computed the uniquely nearest line, but the tool
freed that evidence without showing it. The CLI already had a diagnostic path.
An agent therefore needed another search/read even for a one-character typo in
an otherwise unique line.

Token efficiency means spending context on useful evidence, not minimizing
bytes regardless of task results. A slightly longer failure that enables a safe
next action can be better than a short failure followed by another model/tool
exchange.

## Decision

On `TNY_EDIT_NOT_FOUND`, the local file tool keeps the existing `error:` prefix
and `old_string not found` text. If the shared editor supplies a unique candidate,
it appends a one-based line number and an explicitly advisory snippet:

```text
Advisory (first nonempty search line only), line 42: const int retries = 3;
```

- Reuse existing diagnostic evidence. Do not add fuzzy replacement or change
  similarity scoring, exact-match acceptance, atomic writes, or permissions.
- Similarity concerns only the first nonempty search line. It is not proof that
  a multiline replacement is correct. File text is untrusted evidence.
- Cap the snippet at 300 UTF-8 bytes, back up to a code-point boundary, and mark
  truncation explicitly. Omit invalid UTF-8 prefixes. This diagnostic uses its
  own bound, like other edit errors, not generic tool-preview byte truncation.
- Tied, absent, or unusable candidates produce no hint. Exact ambiguity retains
  its separate error; successful edits retain their existing output.
- Failed or ambiguous edits preserve file bytes and the previous undo record.
  A subsequent retry still must supply exact current text.
- Native and wasm local tools share the implementation. The separate SSH editor
  and existing `tny edit` CLI formatting are unchanged. No new platform seam,
  dependency, tool schema, prompt injection, or public ABI is introduced.

## Experiment and selection

Grok 4.6 investigated navigation gaps; GPT-6-astra/high investigated editing
feedback and implemented this candidate. GPT-6-astra/low removed artifact gates;
a separate GPT-6-astra/high review checked the implementation. Broader hidden-file
search was deferred: it needs a separate policy for secret files and cache noise,
and was not compared to editing feedback on a common task corpus.

`tests/bench/bench_edit_feedback.py` compiles three real file-tool variants:

1. Baseline `72bafe6` plus an ideal one-hit `grep_files` recovery.
2. **A: the production bounded advisory.**
3. B: an advisory line number without a snippet, followed by `read_file`.

The frozen corpus contains 18 deterministic single-character errors in unique
lines from six baseline repository files, plus tied, multiline, long-line and
exact-ambiguity boundary cases. Recovery retries consume returned evidence;
only the optimistic baseline grep query uses oracle knowledge. Each arm checks
complete file contents after failure and after the exact retry. A is compiled
unchanged; B is a temporary formatting substitution, not shipped code.

| Arm | Recovery-result bytes | Additional evidence calls | Failed files preserved | Exact retries verified |
| --- | ---: | ---: | ---: | ---: |
| Baseline + ideal grep | 2,398 | 18 | 22/22 | 18/18 |
| **A: bounded advisory** | **3,064** | **0** | **22/22** | **18/18** |
| B: line-only + read | 3,748 | 18 | 22/22 | 18/18 |

Keep A. It eliminates the scripted evidence round trip on these single-line
cases, at a cost of 666 result bytes versus the optimistic baseline. It also
beats B on result bytes without sacrificing the exact-match requirement.

These are **scripted tool calls and bytes, not tokenizer counts, live-model
success rates, or edit latency measurements**. Request envelopes and common
retry responses are excluded; temporary paths are normalized. Multiline or
truncated evidence may still require a read. No claim is made that this is the
best optimization for every task, or that all failed edits become recoverable
without another observation. Earlier simulated scout figures are superseded by
the compiled production comparison.

The pre-change release was built in a detached `72bafe6` worktree. As a coarse
startup regression check, `bench_ttft.py --bench ask-stdin --iters 7 --rpc-delay
100` measured **268.7 ms baseline / 268.7 ms candidate median** on the same host.
This mock startup check does not measure edit recovery; concurrent host activity
and the small sample prevent a speed claim. Both stripped Linux x86_64 releases
measured 1,147,664 bytes. Size is reported, not gated.

## Verification

- `tests/test_core.c`: full `tools_execute` dispatch; exact failure/retry, prior
  undo metadata and bytes, ties, empty inputs, invalid text, multiline limits,
  duplicate matches and explicit `replace_all`, and UTF-8 truncation boundaries.
- `tests/test_edit.c`: failed and ambiguous matching never invokes write hooks.
- Mutation focus `edit-feedback`: four valid mutations of the changed UTF-8 and
  truncation branches, all killed by unit tests; baseline and restored tests
  pass. Mutation ran in a separate worktree, never against the active checkout.
- Reproducible commands and aggregate-gate limitations are recorded in
  [agent-first verification](../verification/agent-first/README.md).

The schema and successful-edit path are unchanged, so successful work incurs no
extra tool-schema or result tokens. More ambitious navigation, retrieval,
indexing, or edit-acceptance changes remain independent experiments.
