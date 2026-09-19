# Preliminary review dispositions

The actual first review is preserved in [review1-findings.md](review1-findings.md).
It was Claude Fable 5.1, medium effort. Lead's triage governs the dispositions
below. No second review has been run by this implementation process.

| Finding | Disposition |
| --- | --- |
| 1 | Stage provider-owned fields, discard on failure, publish on success. Keep live context/extension ownership. Unit tests preserve provider, URL, key, model and header pointers through key-only Codex and later tier-validation failures; TUI reports retention. |
| 2 | Lead rejected silent fallback. Remembered missing gateways fail closed; explicit selection can override. Native profile fixture verifies this. |
| 3–4 | Auto-detect usable OAuth only. Readers return reason codes without stderr. Explicit Codex reports migration guidance; standalone search still reports broken configured stores without silently changing route. Native login can repair a retired key-only store. |
| 5 | Grok URL fixture override restricted to HTTP(S) numeric 127.0.0.1 with strict authority/port checks. Tests reject remote, userinfo, bad scheme and bad ports, check header separation, and retain actual expired-credential refresh grant assertion. |
| 6 | Remove Cursor snapshot/capture/environment replay; retain legacy scrub/redaction fields. Reject legacy worker payload before child launch. Negative private-worker regression added. |
| 7 | Refuse removed-provider attachment before context changes; resolve available provider configuration before adopting session metadata. Unit regression covers legacy selectors. |
| 8 | Correct setup prompt and boolean argument. |
| 9 | Lead rejected uppercase-only/existing-variable heuristics. Accept POSIX names including lowercase, warn on missing variable, reject raw-key setup and malformed names. Document opaque-name limitations. |
| 10 | Keep explicit missing xAI environment source fail-closed. Fix comments and add no-request/no-refresh regression with a Grok login present. |
| 11 | Remove obsolete process plumbing, comments, Connect test/mutation remnants and stale current documentation. Preserve historical ADRs/evidence and optional MCP imports. |
| 12 | Reserved Cursor/ACP names return UNSUPPORTED; numeric public constants/layouts/exports remain. Update SDK expectation. |

The new provider decision is ADR 0152; credential and failure policies are ADR
0153. The historical ADR 0151 is untouched. Delivery, feature PR, and second/final
review remain the lead's responsibility; this writer makes no commits or pushes.
