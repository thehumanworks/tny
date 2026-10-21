# Agent development map

Start with the outcome, then open one owning seam. Do not read all ADRs or the
whole runtime to make a local change. `AGENTS.md` supplies invariants; current
behavior is in the linked feature contract; ADRs explain decisions when needed.

| Change | Start here | Fast verification |
| --- | --- | --- |
| Native HTTP protocol or recovery | [HTTP source map](backends/openai-source-map.md), `src/backends/openai/` | `make test-openai-errors`, `openai_suite`, local provider fixtures |
| ChatGPT sign-in | `src/core/openai_{login,auth}.c`, [contract](backends/openai-chatgpt.md) | `tests/integration/test_openai_signin.py` |
| Claude connection | `src/backends/acp/`, [contract](backends/acp.md) | `test_acp_client.py`, `test_acp_managed.py` |
| Add/change an extension | `python/tny_ext/`, [plugin guide](extensions.md) | `make test-extensions-python`; explicit-path plugin check |
| Prompt reuse / context stability | `src/core/skills.c`, `learning.c`, native request builder | `skills_suite`, `learning_suite`, `tests/bench/bench_prefix_stability.py` |
| Model-visible tool discovery | `src/core/tools.c`, `src/core/code_python.c` | `code_runtime_suite`, `test_execution_code_mode.py` |
| Search scope / completeness | `core/tools_fs.c`, [contract](adr/0208-truthful-scoped-file-search.md) | `test_search_coverage.py`, `bench_search_coverage.py` |
| Large file/web content | `tools_fs.c`, `tools_web.c`, session result writer, [contract](adr/0212-size-aware-content-returns.md) | `test_read_file_ranges.py`, `test_content_bounds.py` |
| Ranged local file reads | `core/tools_fs.c`, [contract](adr/0211-bounded-ranged-file-reads.md) | `test_read_file_ranges.py` |
| Implement a nested tool | `src/core/tools_*.c`, [catalog](features/mcp-and-skills.md) | owning unit suite + real execution fixture |
| Parallel independent reads | `execution_parallel.c`, `execution.c`, `code_python.c` | `test_execution_code_mode.py` (rendezvous, barriers, spill recovery, timeout) |
| Recover execution output | `session.c` streaming results, `code_runtime.c`, `tools_shell.c` | `test_execution_code_mode.py` (middle diagnostics, resume, limits, binary ranges) |
| Long-session execution transport | `execution_state.c`, `execution.c`, [projection contract](adr/0207-bounded-execution-session-projection.md) | `execution_state_suite`, `test_execution_snapshot.py`, encoder benchmark |
| Run/cancel a code cell | `execution*.c`, `code_runtime.c`, `util/execution_*.c` | `test_execution_code_mode.py`, `test_terminal_cancel.py` |
| Resume/crash recovery | `session.c`, `checkpoint.cpp`, `runner.cpp` | `runner_suite`, session/checkpoint fixtures |
| Durable job records and transactions | `core/jobs_record.cpp`, private `jobs_record.hpp` | `make test-runner-ownership test-runner-mutation`; job corruption fixtures |
| Durable background orchestration | `core/jobs.cpp`, `team_*.c`, `util/jobs_host.c` | job/team integration fixtures |
| Shell DAG scheduling | `shell/tny-workflows.sh`, [workflows](workflows.md) | `make test-shell-workflows` (Bash and Zsh) |
| Bounded workflow learning | `learning.c`, `util/learning_store.c`, [contract](instruction-improvement.md) | `learning_suite`, `learning_process_suite` |
| CLI / TUI presentation | `src/cli/`, `src/tui/` | CLI JSON/help fixtures, PTY tests |
| Public embedding | `include/tny/tny.h`, `src/lib/`, `sdk/` | ABI/conformance/SDK gates |

Unit-suite selectors use `build/tny-test -s SUITE` after `make debug`. Python
integration files run against `TNY=/absolute/path/to/tny` with synthetic local
providers; individual unittest selection works through `python -m unittest`
with `tests/integration` on `PYTHONPATH`. Do not use live credentials to make
ordinary regression tests pass.

## Extension decisions, not another runtime

- Use **Python hooks** for event-driven policy, verification and bounded
  continuation. They are explicit trusted user installations, not code loaded
  automatically from an arbitrary checkout.
- Use **MCP** for external tool services and **run_code** for composing host
  workflows. Search names/descriptions first; retrieve exact schemas only for
  relevant tools. Keep heavyweight tool results out of model context unless needed.
- Use the **native C ABI** only when embedding the existing runtime. Do not
  recreate provider loops in Python/TypeScript SDKs.
- Use **ACP** only for the verified external Claude adapter. Preserve the
  owning-runtime tool/permission bridge and vendor-managed authentication.

Each extension point has one owner and one contract. Prefer extending that owner
to adding a parallel plugin loader, persistence layer, provider bridge, or
self-improvement controller.

For what each verification layer actually guarantees (and what it does not),
use the [compact invariant map](verification/value-density.md#invariant-map).

## Verification ladder

1. State the invariant and add the smallest executable counterexample or bounded
   property check at its owning seam.
2. Run that check and the affected production-process/wire fixture.
3. Run `make test`, `make quality`, and applicable leak/formal gates. Report
   actual failures and unavailable tools separately from passing focused checks.
4. For performance, compare the unchanged base revision and candidate on the same
   machine and toolchain. Output bytes, tokenizer counts, latency, inference cost,
   and task quality are different metrics; never substitute one for another.

The [modernization record](verification/agent-first-modernization.md) tracks the
specific changes, measured evidence and limits for this iteration.

## Historical task briefs

Root-level `HANDOFF.md` and `SWARM_DELIVERY_PROMPT.md` described past deliveries,
not current work or authorization. Their last pre-cleanup copies remain in
[Git history](https://github.com/thehumanworks/tny/tree/656d63a386719211580df8159742b02c0a3a6745).
Dated verification records remain under `docs/verification/`; historical links
and source hashes describe the recorded revision, not today's file inventory.
Editor hook progress is per-machine state and stays ignored; reusable editor
rules still point to `AGENTS.md`.
