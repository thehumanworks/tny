# ADR 0176: Normalize available dictation by default

Status: Accepted

## Context

ADR 0175 introduced a verified LLM rewrite after transcription but made it
opt-in. Users with working dictation credentials expect their transcripts to
be harmonized without additional setup. The opt-in default silently delivered
raw text in both the TUI and CLI.

## Decision

For successful dictation, enable normalization by default using the selected
STT adapter's credential and default model. The existing local availability
check precedes transcription and any normalization request; absent credentials
or unavailable capture/file prerequisites never trigger inference. Remote
normalization failure, timeout or rejected corrections still keep the raw
transcript. No provider I/O happens before a turn or explicit dictation.

Explicit `--no-normalize`, `TNY_DICTATION_NORMALIZE=0` and
`dictation.normalize.enabled=false` (or `normalize: false`) disable it in
that order of precedence. The corresponding explicit enables still work.
An options object without `enabled` inherits the on default. Toolkit
`dictate` remains explicitly raw, as requested by its existing API.

## Consequences

Successful dictations now make a second provider request and may take longer
(up to the configured timeout, 20 seconds by default). Dictation data and
merged dictionary entries are sent to that provider as described in ADR 0175.
`--json` now includes the normalization record by default. Users who prefer
raw transcripts can opt out without changing their login.
