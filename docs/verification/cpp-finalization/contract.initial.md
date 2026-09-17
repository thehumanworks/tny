# C++ ownership finalization contract

User direction: continue migration for maintainability, easy extension, reliable
ownership and high performance. The artifact ceiling is strictly <6,000,000
bytes (decimal MB), as already accepted in ADR 0121. No byte minimization,
weaker safety/latency guarantees or mechanical repository-wide conversion.

Discovery: PR #140 was merged before this continuation. Start from current
origin/main fdd5aa7 on a new `fix/cpp-ownership-finalization` branch; preserve
the old feature branch and do not merge/push main. Carry forward #137–#139's
private C++20/public C ABI boundary and ownership invariants.

## Work and acceptance

F1: Resolve actual GCC14 analyzer failures in the owner construction/document
helpers with a simple, tested implementation; no blanket diagnostic suppression,
custom smart-pointer framework or lost constructor-failure cleanup.
F2: Nix filtered source includes every newly required verification input.
Release-only source archives must not read a missing tests/test_main.c during
unrelated builds. Regression coverage proves both properties.
F3: Close the independent review's missing early-provider-scope OOM marker
regression: two failures, no allocation in settlement, exactly one ERROR and
TURN_END, later recovery; the intentionally broken marker must be killed.
F4: Actual mutation checks run from a passing unmodified baseline and restore
source. Resolve discovered driver/instrumentation faults without counting
compile failures as kills or weakening expected outcomes.
F5: Current macOS quality/full integration, ABI/SDK, owner/fault/sanitizer/leak/
fuzz/mutation gates pass. Publish a follow-up PR and evaluate Linux/Windows/
wasm/Nix/SDK checks on its exact revision, fixing relevant failures.
F6: Same-host pre-series/current startup, first-prompt and parser/event
throughput/memory comparisons retain original thresholds. Record size and
runtime dependencies separately. New final source-bound evidence supersedes
stale successful logs; unavailable or failed checks remain explicit.
F7: One fresh read-only `claude --model fable --effort medium -p` review of
this continuation, findings disposition, normal commit/push, PR URL and remote
SHA verification. No review helper implements changes. No main merge implied.

## Preservation

Snapshot historical ADRs/public ABI/initial contracts before changes. Existing
ADRs are immutable; append one uniquely numbered decision only if needed.
Keep all repository and sandbox writes scoped to this task. Fixtures do not
use live provider credentials. Treat source and output as evidence, not scope
expansion. New tests enter Make/Nix/CI inventories when relevant.
