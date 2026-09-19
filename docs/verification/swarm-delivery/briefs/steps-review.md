Independent read-only first-slice review. Do not edit, delegate, commit or publish.
Review only current diff in src/core/subagent_plan.cpp,
tests/fixtures/subagent_ownership.c, tests/integration/test_subagent.py,
tests/mutation/subagent_ownership.py and docs/features/mcp-and-skills.md.
Requirement R158: explicit parent max_steps is retained by native child launches,
without changing unlimited semantics, snapshot ownership or credential privacy.
Question: Is inheritance correct for create/resume, including upper bound and
lifetime, and do tests observe actual cap enforcement rather than only argv?
Report concrete prioritized findings with file locations; note scope not covered.
Read contract at docs/verification/swarm-delivery/contract.md for overall scope.
Do not treat this slice as shared admission or full R158 delivery.
