# 0190 — Cache-stable context and bounded discovery

Date: 2026-10-19
Status: accepted

## Decision

Keep the existing native provider loop, cache routing and singleton `run_code`
interface. Improve their actual inputs and result path rather than adding a
second context manager or provider-specific cache configuration.

- Sort resolved skill metadata by name after existing root precedence and
  name deduplication. Directory enumeration order is not meaningful prompt
  content. This does not change which root wins a duplicate name; ambiguous
  duplicate names within one root retain their existing resolution behavior.
- Emit fixed eligible learning advice without repeating its changing success
  and failure counters in each system prompt. Keep counters, persistence,
  promotion/demotion thresholds and the empirical-advice disclaimer unchanged.
  Evidence remains in the private learning store. A changed eligible policy
  still changes the prompt immediately; do not freeze or suppress real updates.
- Apply the existing bounded-result/spill contract to MCP discovery, including
  empty-query catalog listing. Full results remain available through result
  handles where a session exists; no new discovery service is introduced.
- Read stored result slices directly rather than loading the entire spill file
  before returning a small page. Preserve byte offsets and in-memory behavior.

## Guarantees and limits

These are deterministic data-path changes, not evidence of improved model
intelligence or paid cache hits. The provider ultimately decides prompt caching.
No request fields, cache identifiers, instruction precedence, permission policy,
public ABI or live-account authentication change. Sorting cannot make different
skill content identical. Stable advice cannot preserve cache reuse when an
actual policy, workspace instruction or tool capability changes.

Verification belongs to existing skill, learning, session and MCP suites, plus
local production-wire learning fixtures. The offline prefix benchmark measures
UTF-8 instruction bytes and checks successful real edit/recovery operations;
it does not estimate tokens, billable savings or task-quality improvements.
The native slice optimization does not add filesystem capabilities to wasm.
