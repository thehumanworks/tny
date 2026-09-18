Implement review fixes for #159/#158 SDKs in isolated feat/swarm-sdk-hardening,
base 31051923fdd44fb75afba6726ce662a0497a79a7. Read AGENTS required docs and lead
contract /Users/tomas/.tny/worktrees/5fe4c489fba3f2e0/docs/verification/swarm-delivery/contract.md.
Own Python/TypeScript workflow implementations/declarations/exports/tests,
shell context-bound tests, docs/workflows.md and ADR0137 addendum. NO native code,
Makefile/Nix, sdk/typescript/scripts/build.mjs (lead owns headers fix), other ADRs
or ledger. No nested agents, push/PR/issue closure/merge.
Independent fixed-commit reviewer b739d967df11861c reproduced:
1. Python native session emits usage 7 input/2 output/cost.25 then raises; result
loses known usage. JS session.ask returns usage then session.close raises; failed
result likewise loses observed usage. Cancellation raises/rejects with no defined
access to already completed/observed accounting. Preserve available usage through
execution and cleanup failure, without changing cancellation rejection semantics;
provide explicit partial accounting retrieval (owned snapshot, no double count).
Late usage during cancellation cleanup must not silently become zero/unknown.
2. Python fields selection json.loads accepts {"keep":1,"omit":NaN} or omitted
1e400 while JS rejects whole-source invalid/nonfinite data. Define consistent
whole-source finite JSON policy before selecting; reject NaN/Infinity and overflow
in omitted fields too. Test selected and omitted invalid fields, malformed/nonobject,
deep nesting, duplicate keys policy and documented integer precision limits.
3. On Python runner failure, result.error keeps traceback frames with composed
prompt including runner's argument. 32 sequential failing consumers retain 8,395,008
prompt bytes. Define useful error diagnostics without retaining those frames; just
del prompt in scheduler is insufficient. Test failing consumer memory/liveness
and cancellation/composition failure; preserve useful error type/message.
4. Add exact-bound/bound-minus-one selection framing/provenance/base64 cases; one
mixed raw/summary/fields/artifact/no-context fan-in. Shell's supposed UTF8 test is
ASCII; add real multibyte exact complete-input boundaries matching shared option.
5. Artifact metadata alone is NOT model-dereferenceable. Current inline base64
slice is portable and bounded; do not imply automatic isolated/SSH retrieval.
Implement an explicit safe custom-tool adapter if needed to meet issue159's actual
bounded access criterion, without remote paths or silently fetching. Otherwise
keep precise capability docs and prove inline slices reach actual native fixture
request under a distinct cwd, not merely internal render tests.

Preserve existing SDK API defaults except additive documented fields/accessors.
Normal run accounting sums each task once, unknown stays unknown. Native fixture
should include errors after observed usage and multiple snapshots in JS where
possible. Run SDK/shell/typing/style tests and compare baseline regression before
claiming fixes. Commit coherent changes and return SHAs, actual commands/exits,
criterion mapping and remaining limits. Lead owns integrated root gates and Nix.
