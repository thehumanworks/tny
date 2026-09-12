# 0087 — Explicit subagent contract and private child launch

Date: 2026-09-11
Status: proposed; implementation and behavioral verification pending
Requirements: R123.1–R123.5; verification contract docs/verification/open-issues-2026-09-11/contract.md, amendment A1.
Relates to ADRs 0031, 0058, 0063 and 0081. Existing finalized ADRs and index remain unchanged.

## Decision

Keep generated 16-lowercase-hex durable session IDs. Reject a supplied id on create rather than interpreting it as resume; message/inspect/lifecycle use returned IDs. Lifecycle observes the persisted state and actual writer lock. Correct unsupported relationship/configure and queued-message claims in current feature documentation.

Extend the existing error string with stable SUBAGENT_* classes and safe actionable guidance. Never relay raw child failure bodies or secret-bearing configuration through argv, tool diagnostics, session error records or extension diagnostics. Transfer the resolved configuration and credentials only into a private child environment/context. Preserve permission/tool-profile ceilings, ephemeral semantics, correlated extension events and process ownership. Use the existing host OS process seam; no credential store, alternate provider loop, daemon or alias namespace is introduced.

## Alternatives and consequences

Adding named aliases would require a second identity/index/collision mechanism not required by supported-schema acceptance. Keeping create-as-resume is ambiguous and unsafe. Scrubbing arbitrary provider stderr is brittle; structured safe error classes avoid propagating such text while preserving the failing stage. Reusing the resolved child configuration prevents stale remembered provider settings and flag-selected credentials from diverging.

A narrowly additive environment-valued CLI option is allowed only if existing configuration precedence cannot carry a secret-bearing endpoint privately; it must have parser/help/schema tests. Actual implementation decisions and validation references will be finalized by the primary writer after independent code review. No result is yet claimed.

## Validation

Baseline named/automatic reproduction: docs/verification/open-issues-2026-09-11/artifacts/subagent-before.json. Required final checks and controlled faults are C123, C-G1/C-G2/C-G3 and M123.1–M123.5. Platform checks not run remain blockers rather than passes.

## Current implementation refinement (still proposed)

The shared subagent module owns validation, bounded child stdout, outcome classification and actual stored-state observation. `tny_process_spawn` and `tny_process_self_path` live in the existing host-OS seam and return ENOTSUP on wasm. The additive `--base-url-env` selector carries flag-resolved and builtin subscription endpoints privately when settings cannot; the prompt is supplied on stdin. TNY_NESTED/MODE and TNY_TOOLS preserve creator ceilings without mutating the parent environment. Child stderr is discarded rather than heuristically scrubbed.

This explicitly supersedes the subagent portions of ADR 0022 (hidden/unsupported under SSH, never accidentally run local tools) and ADR 0062 (stable SUBAGENT_UNSUPPORTED_CONTEXT for direct hidden-profile calls). No finalized ADR file is changed. The blocking subagent wait does not call the runner control pump, which could re-enter a backend with an active tool frame; the existing cancellation probe remains its cancellation input. A child receives SIGINT, then a bounded owned-tree fallback after the child CLI's shared five-second deadline plus a one-second margin. An enclosing hard stop can still leave an honest stale record; it never becomes invented success.

The new wind-down regression fails against the three-second implementation and passes with the corrected deadline (artifacts/resume-20260911/R002-wind-down-before.json and R004-wind-down-after.json under the canonical verification directory). These are local observations, not cross-platform or integrated completion.


## Known launch-path diagnostic preflight (still proposed)

The process seam stats its absolute executable before spawn and rejects already missing, non-regular or entirely non-executable paths. This preserves LAUNCH_FAILED diagnostics when a conforming spawn implementation instead defers an exec failure to child exit 127. It is not authorization or atomic path binding: replacement/interpreter/kernel failures after the check still use actual child status, and a real child's exit 127 must not be reclassified as a known launch failure. The Linux Valgrind missing-path regression exposed the distinction; A5 retains the original oracle and adds a directory case, instrumentation-specific controlled fault and fresh review before jobs reuse. No blanket suppression or failing-suite reclassification is allowed.
