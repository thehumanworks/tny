# Native HTTP provider verification

## Contract

Implement ADR 0152 on `feat/openai-only-providers`. Preserve the public C ABI,
event schema, native tools/MCP, sessions/isolation, jobs/teams/workflows and
Python/Node SDKs. No vendor agent executable may be discovered or launched.
No tnytty product changes. No live inference, commits, pushes or review agents.

Required evidence: release and active library compile, `make test`,
`make quality`; streaming text/tools on both wires; OpenRouter/AIProxy generic
environment profiles; removed selector/flag/config diagnostics; ignored Claude
artifacts and sentinel vendor binaries; Codex OAuth login/refresh/mock wire;
Grok public Responses and subscription chat wire. Keep existing shared coverage.

`baseline.json` records the 1,147,664-byte clean same-toolchain baseline supplied
by the lead. This is a size comparison, not a speed benchmark.
Final results and dependency measurements are recorded after the gates finish.
Build concurrency is bounded to eight workers. Tests use isolated synthetic
credentials and local mock servers. Logs must contain no inherited secrets.
