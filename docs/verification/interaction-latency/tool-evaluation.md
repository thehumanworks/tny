# Tool-task success and token efficiency

This follow-up tests generated tool workflows, rather than using interpreter
speed as a proxy for agent quality. It uses the existing production CPython
cell and C virtual capability host, with benchmark-only adapters for host shell,
Node JavaScript and Monty 1.4.0. The adapters are not production runtime changes.

## Protocol

- Model: `gpt-6.1-sol`, low effort, public Responses HTTP/SSE using the saved,
  consented Sign in with ChatGPT login. No provider token enters child environments.
- Six published repository task families: joining two files, pagination through
  empty pages, editing only changed files, bounded retries, Unicode scalar
  handling and deterministic topological sorting.
- Four arms × six tasks × three generated samples = 72 programs. Every program
  executes against three fixtures unseen in its prompt (216 initial executions).
  The fixtures are already published in this repository; this is not a novel
  held-out benchmark or a claim about general coding ability.
- All arms receive the same task, complete five-tool catalog, exact output path
  and execution limits. Language-specific API help differs. Arm order rotates;
  two independent inference requests overlap, but program execution is serial.
- A sample passes only if **all three variants** have valid tool arguments, all
  required calls, exact typed output files, successful execution, the prescribed
  completion line and at most 64 calls. Retry traces must have the required
  per-ID call counts; output correctness alone cannot hide extra retries.
- A failed program gets at most one repair with its program and observed errors,
  never expected answers. Repair tokens and unsuccessful attempts count toward
  tokens per successful sample. Each repair replays fresh fixtures.
- Input and output usage come from provider responses; output includes reasoning
  tokens. Cached input and reasoning counts are separately reported. One sampled
  program's token cost is counted once, not multiplied by its three executions.
- Before inference, all 24 hand-written reference programs must pass all three
  variants. Negative controls reject a program that only prints success and a
  program that passes an array where tool arguments require an object.

The ordinary Python arm runs directly in the production embedded interpreter.
Shell can choose shell/Python/Node subprocesses; JavaScript runs in Node; Monty
uses a fresh external worker pool. Their calls return through a Unix-socket
adapter to the **same C tool host**, serviced on the cell's owning main thread.
All arms retain the same tool schemas, result strings and oracle. Adapter
startup/IPC overhead is not evidence about hypothetical native integrations.

Host modes have ordinary host authority and no added network restrictions.
Temporary working directories and minimal environments avoid passing credentials;
they are not security sandboxes. Monty has only explicitly provided host tool
functions, so its sandbox cannot offer arbitrary ambient network/host access.
No default authority settings were changed.

## Results

The retained run completed with exit 0. All **72/72 programs passed on the
first attempt**, across **216/216 executions**. There were no repairs, invalid
tool calls or missing usage records. Each arm made 234 logical tool calls across
its 54 executions, so none achieved lower tokens by skipping required work.

| Arm | First / final task success | Input tokens | Output tokens | Total tokens / successful task |
| --- | ---: | ---: | ---: | ---: |
| Embedded Python | 18/18 / 18/18 | 10,443 | 3,519 | 775.7 |
| Host shell | 18/18 / 18/18 | 11,613 | 3,870 | 860.2 |
| Host Node JavaScript | 18/18 / 18/18 | 10,911 | 4,001 | 828.4 |
| Monty | 18/18 / 18/18 | 10,875 | 4,177 | 836.2 |

Total retained usage: **43,842 input + 15,567 output = 59,409 tokens**.
The provider reported zero cached input tokens and zero reasoning tokens in
this run. Counts include the complete task/catalog/interface instructions;
they are not just source-code token counts. All shell samples selected a
Python subprocess with imports and shell setup. Their extra tokens therefore
measure this interface's overhead, not an advantage of writing POSIX pipelines.
JavaScript's median output-token count was lower than Python's (168.5 vs 177),
but its aggregate output cost was higher; median source length alone would
misrepresent total task cost.

**Decision:** retain embedded CPython as the default. It completed the same
work with the lowest aggregate tokens in this small suite (roughly 6–10% fewer
than the alternatives) and the earlier production-path timing comparison also
favored it. No native shell, Node or Monty replacement is promoted. This does
not establish a general runtime-quality ranking: the suite has a ceiling effect
(all arms pass) and language-specific prompt/setup costs differ. Host shell
remains useful when a task requires installed tools; Monty's isolation remains
a separate design benefit that these tasks do not quantify.

Artifacts: [protocol and order](tool-eval/manifest.json),
[toolchain and fixture hashes](tool-eval/environment.json),
[reference controls](tool-eval/controls.json),
[generated programs and execution traces](tool-eval/samples.json),
[summary](tool-eval/summary.json), and
[expanded metrics](tool-eval/metrics.json). All generation prompts and adapters
were frozen during the retained run; their recorded hashes were verified.
`make lint-py` and Ruff formatting checks passed for the added helpers. The
production patch's broader checks and reproduced baseline gate failures remain
as recorded in [the main report](report.md#verification).

## Excluded pilot

An interrupted pilot omitted the common instruction specifying `output.json`,
so several otherwise-correct programs guessed an absolute output path. It also
left JavaScript's print-newline semantics implicit. Both instructions were made
explicit before the retained run. The pilot is excluded from all comparisons;
16 scored programs and 21 requests with saved usage remain in
`tool-eval-pilot-incomplete/`. Other requests were in flight or completed in a
queue when stopped, so the pilot artifact is not a complete plan-usage ledger.
The corrected run starts fresh; none of its prompts changed during collection.

## Reproduce

Requires the normal production build, host Node/Python and experimental Monty:

```sh
make python-cell-bench
python3 -m venv /tmp/tny-experiments
/tmp/tny-experiments/bin/pip install pydantic-monty==1.4.0
# Local reference controls only; no inference:
python3 tests/bench/interaction/tool_eval.py --output /tmp/tool-controls
# Explicitly authorized live requests consume the signed-in plan:
build/tny --provider openai status
python3 tests/bench/interaction/tool_eval.py --live --repeats 3 --output /tmp/tool-eval
python3 tests/bench/interaction/tool_eval_metrics.py /tmp/tool-eval/samples.json
```

A workstation sandbox must allow local Unix sockets for the experimental
adapters. The first local control attempt was blocked at socket bind; it made
no inference requests. Controls passed when run with that OS permission.

## Limits

This measures one generated program plus at most one repair, not an entire
interactive agent session. It excludes production tool discovery, conversation
history, user ambiguity, cache persistence, cancellation and long-horizon edits.
The small sample and easy published tasks cannot establish equivalent reliability
or a general language ranking. Prompts are parallel contracts, not token-identical;
extra host setup instructions are part of the tested interface cost. Generated
programs and traces are retained so language choices and failure modes can be
reviewed. Provider variation and reasoning length can change token rankings.
