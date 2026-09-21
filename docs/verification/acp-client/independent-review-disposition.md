# Independent review disposition

A separate Claude Sonnet 5 review inspected the bridge and process seam using read-only tools. The first bounded attempt was interrupted; the resumed, no-more-tools report completed successfully. No claim is based on the interrupted attempt.

- **Human permission wait versus tool deadline:** actionable parity concern. A parked approval must not consume the execution timeout, and approval should establish a new execution deadline. Resolved: approval waiting is excluded and execution starts a fresh deadline; the six-case fake-clock regression passed in audit-build2.
- **Catalog discovery before an active turn:** not a defect. MCP clients must discover tool metadata while setting up a session, before a prompt. The private same-user Unix-socket boundary applies; tool execution separately requires the owning active turn. No code change justified by this finding.
- **Empty newline as liveness probe:** not a valid JSON-RPC/MCP liveness request. Empty-line tolerance is not evidence of a lost valid request; the protocol ping method is available. No code change justified by this finding.

The independent review did not establish additional process-lifetime bugs. Parent live audits separately identified strict MCP configuration, duplicate execution events, approval authority, and usage reporting; their acceptance evidence belongs in the final verification report.

The later managed-job source review identified overly broad native-only guards. Managed ACP parity now uses per-child required-authority checks, immutable launch selectors and cleanup-sensitive receipts. Fourteen focused execution tests passed, including manifest activation and failed-prompt context replay. Final combined gate results are recorded separately.

The final managed-source review found three command fidelity defects: guarded relative executable paths, equivalent ACP alias inheritance, and literal delimiter arguments. They were corrected with explicit guarded-path validation, canonical selector comparison and counted child argv serialization; ownership and integration regressions cover the fixes.

## Delivery accounting review

The final live managed test found that the job log reader ignored the explicit
unreported-token marker. The reader now preserves unknown counts. A failing
pre-fix managed regression and passing post-fix case establish the correction;
the complete 19-case managed suite and five native usage/budget cases verify
both sides of the behavior. The metadata-only live observer then confirmed
unknown token summaries for both successful Sonnet workers.
