# 0175 — Dictation transcript normalization on the STT subscription, verified in C and in Lean

Date: 2026-09-26
Status: Accepted (live effort probe and fast-tier benchmark pending; see Verification)
Extends: [0079](0079-provider-independent-dictation.md). Related: [0009](0009-reasoning-effort.md),
[0010](0010-fast-tier-capability.md), [0171](0171-formal-verification-smt-lib.md),
[0169](0169-desktop-effort-folder-markdown-and-verified-turn-status.md) (Lean golden-table pattern).

## Context

Dictation (ADR 0079) inserts the raw STT transcript. Transcripts mangle
project vocabulary: `kube cuddle` for `kubectl`, `tiny` for `tny`, `jeff` for
`Jev`. Neither STT adapter accepts a vocabulary or prompt hint: the ChatGPT
transcribe endpoint has no model parameter and the xAI REST STT contract has
no model parameter or versioned model ID. Post-hoc normalization is the only
lever. It must not become a way for a model to rewrite what the user said, must
never lose or truncate a transcript, and must not touch the conversation
provider, session or workspace.

## Decision

**One structured rewrite on the STT subscription's own small model.** After a
valid transcript, the service optionally makes one request from the credential
the selected STT adapter already resolved:

| STT adapter | Default model | Wire |
| --- | --- | --- |
| `codex` | `gpt-6-luna` | ChatGPT credential chain and trusted gateway, `POST {codex base}/responses`, `store:false`, `stream:true`, structured output via `text.format` (`json_schema`, strict) |
| `xai`, API key (`--xai-api-key`, `XAI_API_KEY`, `xai` profile `api_key_env`) | `grok-4.7` | `POST https://api.x.ai/v1/chat/completions`, streaming, `response_format` `json_schema` |
| `xai`, Grok login | `grok-4.7` | `POST https://cli-chat-proxy.grok.com/v1/chat/completions` (streaming-only), `X-XAI-Token-Auth`, `x-grok-client-version`, `x-grok-model-override: <model>`; JSON by instruction only, parsed strictly |

The model is never the conversation model: `dictation.normalize.model` (a
string or per-adapter object) and `TNY_DICTATION_NORMALIZE_MODEL` override the
adapter default. The normalizer never resolves the conversation profile,
creates a session, loads tools or reads workspace content. The request carries
only fixed instructions, the transcript and the merged dictionary's entries.
It is a second pre-turn provider call from the STT credential, like the STT
upload itself: startup still makes no provider I/O, and nothing is recorded as
conversation usage or history. The Grok proxy does not get `response_format`
until a live probe shows it is honoured.

**Structured proposal, deterministic disposal.** The model returns
`{"text", "corrections": [{"span", "replacement", "reason"}]}` with `reason`
one of `dictionary`, `case`, `punctuation`, `number`. The C verifier accepts the
rewrite only when every check passes, in this order (the first failure is the
recorded reason):

1. `text` passes `tny_dictation_text_valid` (nonblank, ≤ 64 KiB UTF-8, no
   terminal controls);
2. at most 64 corrections;
3. each correction has a nonempty span and satisfies its declared reason:
   `case` — equal after ASCII lower-casing; `punctuation` — equal semantic
   tokens (lower-cased, ASCII punctuation removed, except that a token holding
   a digit keeps its inner punctuation so `3.5` ≠ `35` and `-5` ≠ `5`);
   `number` — the replacement is a numeral (`123`, `1,234`) whose value the span
   spells as an English cardinal below 10^12 or as a differently grouped
   numeral; `dictionary` — the replacement, without surrounding sentence
   punctuation, is a dictionary word (byte-exact for `"case": "exact"`, else
   ASCII case-insensitive) and the span is one word or one of that word's
   aliases;
4. applying the corrections in order, each at the first occurrence after the
   previous one, reproduces `text` exactly (no unlisted edits);
5. the semantic-token Levenshtein distance from the transcript is at most
   `3 + n/4` for `n` semantic transcript tokens.

**Dictionary.** `~/.tny/dictionary.json` merged under
`<workspace>/.tny/dictionary.json`, the project winning per word (the
`settings.json` convention). A flat `"word": "context"` object, or per word
`{"context", "aliases" (≤ 8), "case": "exact"|"insensitive"}`. At most 64 KiB
and 256 words per file, words ≤ 64 bytes with a letter or digit and no
surrounding space, contexts ≤ 256 bytes, no control characters, no duplicate
or unknown keys. A missing file is empty; an invalid one skips normalization
(`dictionary_invalid`) and keeps the raw transcript. Schema:
`schemas/dictionary.schema.json`.

**Fail open, opt in, cancellable.** Off by default. Enable with `tny dictate
--normalize`, `TNY_DICTATION_NORMALIZE=1` or settings `dictation.normalize`
(`true`, or an object with `"enabled": true`); `--no-normalize` forces it off.
Missing credential, invalid configuration or dictionary, transport error,
timeout (default 20 s, `timeout_seconds` 1–120, one deadline for the whole
normalization), HTTP rejection, malformed output and verifier rejection all
deliver the raw transcript with a bounded `skipped_reason`. The TUI shows
**Normalizing** after **Transcribing**; **Esc**/**Ctrl-C** (or an approval taking
the keyboard) stops the rewrite and inserts the raw transcript. The CLI keeps
its rule that an interrupted command prints nothing and exits 130. The job
uses the shared HTTP seam and `tny_dictation_step`/`tny_dictation_fd`, so the
event loop and wasm fetch transport drive it like the STT request.

**Effort and tier.** The default effort is canonical `off`, sent as `none`
(ADR 0009 mapping; `light` → `low`; provider tokens such as `minimal` pass
verbatim; `"omit"` sends no field). HTTP 400/422 while the field was sent is
retried once without it, never a failed normalization. `fast: true` sends
`service_tier: "priority"` on the codex wire only; it is an explicit
normalization setting, never inherited from the conversation's `--fast`, and
it is off by default until the benchmark below justifies a change.

**Lifecycle and proofs.** Every normalization transition in `dictation.c` goes
through `tny_norm_step`, a C translation of `Dictation.Lifecycle.step`. The Lean
4 project `tests/formal/dictation` proves:

* `applyAll_sound` — an accepted rewrite differs from the transcript only at the
  listed spans, with identical gaps in the same order (`Decomp`);
* `verify_ok_sound`, `identity_ok` — acceptance implies every check above; the
  unchanged transcript with no corrections is always accepted;
* `deliver_raw_or_verified`, `deliver_valid`, `deliver_decomp` — the delivered
  text is the raw transcript unless an accepted rewrite exists, and is valid
  whenever the transcript was;
* `case_only_preserves_folded`, `empty_dictionary_rejects_dictionary`;
* `merge_lookup`, `merge_words`, `merge_nodup` — project-wins dictionary merge;
* `off_never_requests`, `at_most_two_requests`, `transcript_never_lost`,
  `cancel_normalizing_delivers_raw`, `effort_retry_once`,
  `normalized_needs_acceptance`, `normalizer_events_progress` — lifecycle
  invariants over all reachable states;
* `levDP_eq` — the row-by-row Levenshtein equals the recursive specification
  (`@[csimp]`, used by the exporter).

`lake exe export golden` writes `lifecycle.tsv` (all 180 states × 8 events),
`dictionaries.tsv` and `verify.tsv` (1,332 generated and targeted proposals
covering every verdict). The C unit suite replays all three against `tny_norm_step`,
`tny_dictionary_parse`, `tny_norm_proposal_parse` and `tny_norm_verify`; the CI
`lean-proofs` job rebuilds the proofs, regenerates the tables and fails on any
difference. As with ADR 0171, the proofs cover the specification and the
golden tables tie the C implementation to it on those inputs; they are not a
proof of the C code, the network, or the model.

## Consequences

Users can register their vocabulary once and get corrected identifiers,
casing, punctuation and numerals without a new credential. A normalizer model
can only make the listed kinds of change, only at the spans it lists, and only
a bounded number of them; anything else, including a slow or failing
provider, costs at most the configured deadline and leaves today's raw
behavior. Spoken punctuation ("comma"), decimals, ordinals and years are not
converted; such rewrites are rejected rather than trusted. A dictionary
correction can still replace a single word with a dictionary word from
context, bounded by the edit budget; aliases are where precision comes from.
The libtny toolkit keeps raw transcripts (no ABI change).

## Verification

Local (Linux x86_64, 2026-09-26), synthetic credentials and loopback mocks only:

* `lake build` (Lean 4.30.0) proves every theorem above with no `sorry`;
  `lake exe export golden` reproduces the committed tables.
* `make test-dictation`: **18 unit tests** (11,986 assertions, including both
  golden replays and a 2,000-case banded-vs-full Levenshtein cross-check) and
  **51 integration tests**. New fixtures cover both adapters and both xAI
  credential paths: off by default with credentials present, accepted
  rewrites with the merged dictionary, rejected rewrites (unlisted,
  inadmissible, invalid text, malformed/fenced output), HTTP 401/429/500/503,
  timeout, effort rejected and retried without the field (and no retry when no
  field was sent), invalid dictionary/configuration with no request, SIGINT with
  no stdout, and TUI Esc mid-normalization inserting the raw draft.
* Targeted mutation run (`--focus dictation-normalize`): see the PR for the
  final counts.

Pending, requiring explicitly authorized live accounts (not available to this
change):

* the lowest effort value `gpt-6-luna` accepts on the ChatGPT Responses wire and
  whether `grok-4.7` accepts `reasoning_effort` on the API and the proxy, plus
  whether the proxy honours `response_format`; the retry-without-field path
  makes any answer safe in the meantime;
* the fast-tier benchmark (p50/p95 end-to-end normalization latency, default
  versus `priority`, for 5 s, 30 s and 120 s transcripts, and the per-call cost
  delta). Until then `fast` stays off by default.
