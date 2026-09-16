# Phase-two integration plan

2026-09-16, before this continuation's phase-two implementation. Root owns
this checkout. Adapt finished commits 4c62c66 and 1b2b89a from the separate
migration/cpp-series branch; do not modify that worktree or copy its active
uncommitted edits. All P2-I1..P2-I6 remain required.

The user requested commit/push after all three issues. Preserve that delivery
order: local phases compose in dependency order and the combined publication
provides hosted gates. Do not claim hosted-before-next-phase execution; final
platform guarantees remain required. Clarification about early publication
was offered, but no early-push authorization was received.

Independent design reviewer /root/phase2_integration_design inspected the
finished commits and current APIs. Use util/ownership.hpp or json/ownership.hpp,
owned/make_owned, and explicit parse(data,size); preserve the root parsers and
C/C++ build discovery. Port owned events first, then async registry/call
ownership, then allocation-free emergency provider settlement. Existing public
headers/exports remain unchanged. C++ allocator control blocks participate in
fault injection; no global operator new replacement.

Tests: actual retained event fields/queue ordering, exact terminal settlement,
exhaustive deterministic owner/control-block fault indexes, reserve recovery,
real worker cancellation/unregister/teardown/generation behavior, C/Python/Node
clients, ABI, sanitizers, mutation oracles, leak checks and Linux TSan. The
variable tail in the public fault-sweep discovery must be reconciled, not
called exhaustive merely because its minimum-count prefix passed.

Adapt runtime mutation/ownership targets to current Make variables. All C++
objects in test-counter binaries must share TNY_ALLOC_TESTING; live ownership
counters are process-wide atomics independent of per-call fault scopes.

Current phase-one source manifest: ../cpp-series/artifacts/continuation-phase1-source.json.
Initial patch SHA256: 601b144e82ab978d65bd9f6648e285d97e27de220e07f8c0719fe216b6ee7001
