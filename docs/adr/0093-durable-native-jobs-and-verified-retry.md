# 0093 — Durable native jobs with owned execution and verified selective retry

Date: 2026-09-12
Status: proposed; integrated verification pending
Requirements: R124.1–R124.8, R127.8 and their invariants; canonical verification contract A11.

## Decision

Use private per-job versioned records, live owner locks, short nonblocking state transactions and immutable attempt snapshots. A detached supervisor invokes the existing real tny CLI for canonical ask events or shared image operations; no second provider or agent loop. Staged descriptor mappings carry private payload/ack and the owner handle without fd collisions. After child spawn the submitter never writes competing terminal metadata; uncertain handshake reports its durable job ID for status/cancel rather than resubmitting.

Cancellation is recorded per attempt/item and linearized against the queued launch claim. Only actual live child handles may be signalled and reaped; stored PIDs never confer authority. An explicit parent-relationship watcher feeds existing request/tool cancellation probes. Missing worker ownership yields interrupted/cleanupunknown, not fabricated success/cancelled cleanup. Output-limit failures cancel the child rather than discard a tail and claim success.

Per-output reservation locks serialize canonical path claims across submitters. A reclaim retains that reservation lock throughout a nonblocking owner-lock probe, state check and rewrite; an active/uncertain owner always denies reclaim. No blocking owner probes while holding reservations. Selective retry resets new-attempt controls, validates all carried successful session/log/result or manifest/artifact hashes before spending, and never regenerates a successful item silently. Metadata/diagnostics exclude credentials; privacy-opted-out prompts/references remain one-run memory-only and require explicit resubmission for retry.

## Alternatives and consequences

A daemon/database or duplicated provider loop adds unnecessary lifetime/API complexity. Stored-PID signalling risks unrelated processes. Permanent reservation ownership after success blocks legitimate later iteration; release claims at terminal state and verify success hashes before a later retry instead. Short private atomic JSON projections plus immutable attempt records provide explicit crash states without pretending two-file or cross-process atomicity. Cooperative parent-loss cleanup cannot prove all stalled descendants exited; that case remains explicitly unknown.

## Validation

Design reviewed independently (V3 andV4), with the final nonblocking-owner-probe and queued-abandonment corrections fixed in A11 before code. The corrected canonical event/process path has its own fresh code review, native checks and mutation set before reuse. Required jobs checks exercise actual submitter/worker death, queued/running cancellation, unrelated sentinels, collisions, concurrency counters, carried-success integrity, privacy, permissions and all offered surfaces. Final manifest/export/job references, wasm rejection, native platform gates and critical mutations remain mandatory; no completion is claimed by this proposed ADR.
