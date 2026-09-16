# Continuation contract — 2026-09-16 04:25 UTC

User request: complete and test #137, #138 and #139, then commit and push.
Reuse the phase contracts and all 18 behavioral invariants. Current delivery
is the feature branch; no main merge or issue closure is implied. Prior
claims of special reviewer/model instructions describe earlier sessions and
are not additional authorization in this session. Fresh bounded review is
required for new implementation decisions under the installed skill.

Baseline: ed355a953c3c73aa78d0ba99f91e61209a0fa9eb. Existing dirty files:
.github/workflows/ci.yml, Makefile, tests/abi/compat0_consumer.cpp,
tests/integration/libtny_custom_tools_cpp.cpp,
tests/integration/libtny_host_services_cpp.cpp, and untracked
 tests/mutation/parser_ownership.py. These are existing issue work and are
preserved. Other active sessions own tny-cpp-series and tny-cpp-phase3;
do not mutate those worktrees or their live tests. Integration source remains
a pending coordination question.

This record precedes this session's first implementation edit; it does not
claim to precede the inherited implementation. Historical review timing must
be reconciled honestly. Initial phase contracts are unchanged. No explicit
native-goal request was made; get_goal returned null and creation is not
authorized by the higher-priority tool rule.

Immediate slice, owned by root: repair the unique source anchor in the parser
mutation driver; include test-parser-ownership in Nix test/source inventories.
P1-I1/I3/I5 checks: unmodified instrumented baseline passes; all four mutants
compile and fail their behavioral oracle; Python formatting/lint and quality
pass; Nix inventory includes target and files. Existing process and API behavior
must remain unchanged. Independent reviewer /root/phase1_review approved the
bounded plan before edits. No new architectural decision is introduced.

Remaining gates: all phase contracts, integrated native/wasm/platform suites,
ABI/SDK/allocator/leaks, runtime and process mutations, performance reports,
review findings, source-bound evidence, final commit/push and remote SHA check.
