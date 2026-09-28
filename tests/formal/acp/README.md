# ACP admission contract model

Standalone Lean 4.30.0, using Std only (no Mathlib). Run from this directory:

```sh
export PATH="$HOME/.elan/bin:$PATH"
lake build
lake exe export golden
```

`Acp.lean` implements canonical uint32 stable SemVer parsing, ASCII build
metadata validation, exact adapter identity admission, and lexicographic ordering
with minimum `(0,75,1)`. Build metadata is discarded, not compared. Missing fields
are `Option.none`. No prereleases, trimming, prefixes, or permissive numeric parsing
are accepted. Components are checked for ASCII digits and canonical spelling
before numeric conversion.

General theorems include `exact_identity`, `old_rejected`, `stable_accepted`,
`upward_closed`, `positive_major_accepted`, `build_ignored`,
`build_order_unchanged`, `parsed_build_ignored`, and `missing_rejected`.
`minimum_accepted` is kernel-checked computation (`cbv`, not `native_decide`).
Acceptance theorems for arbitrary strings explicitly require successful parsing;
they do not claim every natural-number tuple has a valid uint32 spelling.

The lifecycle is an abstract tools-only adapter contract, **not a proof of C**,
JSON transport, process isolation, or actual adapter tool behavior. `Authority`
is proof-carrying: it records admitted fields, protocol 1, loadSession support,
and the mandatory tools-only policy. Only a successful `initHandshake` creates
one. `step` creates sessions only from initialized state, resumes only with
loadSession, and prompts only from ready. Unexpected events leave state unchanged;
a failed resume disconnects. Every connect discards previous authority and checks
the new fields. Disconnect retains nothing.

`reachable_safe` proves admission, protocol, and tools-only invariants for all
reachable active states (indeed all representable authority values).
`disconnect_clears`, `reconnect_revalidates`, `identity_change_clears`,
`downgrade_clears`, and `bad_protocol_rejected` establish reset/revalidation.
`failed_cannot_prompt` quantifies over arbitrary subsequent session/prompt traces
without a fresh connect. `resume_requires_load` proves the capability guard.
These proofs have no `sorry`, custom axioms, or native-decision shortcuts.

`Model.lean` models requested-model selection (ADR 0181): the adapter catalog
never gates a request. `catalog_irrelevant_config`/`_legacy` prove that swapping
the catalog changes neither whether a turn proceeds nor its model;
`prompt_is_requested`, `config_requires_confirmation`, `rejected_fails`,
`unusable_selector_fails` and `annotated_iff_unlisted` pin the rest. Export
writes `golden/models.tsv` (`selector`, `strict`, `wanted`, `accepted`,
`annotated`; strict agents reject IDs outside the fixture catalog), replayed by
`test_model_selection_matches_lean`. Only `propext` is used.

`floor_iff` proves that the production numeric comparison is the specialization
of lexicographic order to the minimum. `Export.lean` deterministically writes
34 version and 18 handshake fixtures:

- `golden/versions.tsv`: `name`, `version`, `accepted`.
- `golden/transitions.tsv`: `name`, `version`, `protocol`, `load`, `resume`, `accepted`.

`name` is the actual adapter identity, not a case label. Empty name/version cells
mean missing fields; preserve empty cells and whitespace. Booleans are `1`/`0`.
There is no TSV escaping because fixture strings contain no tab/newline characters.
For transitions, accepted means reaching ready after initialize followed by new
or resume, not merely accepting the identity/version. Fixtures are replay inputs
for parent-owned C/integration tests; they are not a substitute for the general
proofs, and this package alone does not establish C conformance.
