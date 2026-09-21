# SDK workflow examples

Two complete workflows, each written twice — once against the Python SDK and
once against the TypeScript SDK — so you can compare the ergonomics directly.
They are scripts to copy from, not a framework: everything beyond the `tny`
package lives in one small helper module per language.

| Workflow | Python | TypeScript | Shape |
| --- | --- | --- | --- |
| Auto-research | [`python/auto_research.py`](python/auto_research.py) | [`typescript/auto_research.ts`](typescript/auto_research.ts) | plan → parallel investigators → synthesise → critique; the critic's gaps become the next round; a retro writes lessons the next run reads |
| Code generation | [`python/codegen.py`](python/codegen.py) | [`typescript/codegen.ts`](typescript/codegen.ts) | decompose → architecture → a generation DAG built from the decomposition → review → host-run verification with a bounded fix loop → retro lessons |

Both languages share the role prompts in [`prompts/`](prompts) and the same
lessons files, so a lesson learned by a Python run is recalled by a TypeScript
run and vice versa. Edit a prompt file to retune a role; no code changes.

## Run them

The SDKs embed the native OpenAI-compatible backend and take the endpoint and
key explicitly ([docs/sdks.md](../../docs/sdks.md)). The examples read them
from the environment:

```sh
export OPENAI_API_KEY=...                          # required
export OPENAI_BASE_URL=https://api.openai.com/v1   # default
export OPENAI_WIRE_API=chat                        # for servers without the Responses API
```

### Models and effort

Each role runs on the model and reasoning effort its tier names in
[`models.json`](models.json), shared by both languages:

| Tier | Model | Effort | Roles |
| --- | --- | --- | --- |
| `fast` | `gpt-5.6-Luna` | `light` | research investigators (the wide fan-out), both retros |
| `balanced` | `gpt-5.6-sol` | `medium` | research planner and synthesiser; codegen decomposer, generators, first fix attempt |
| `critical` | `gpt-6-Astra` | `high` | research critic (it gates the loop and writes the next round); codegen architect (every generator builds on its contract) and reviewer; any fix attempt after the first has failed |

The strongest model is spent only where one mistake is multiplied downstream
or where cheaper attempts have already failed; everything wide or routine
stays on the cheaper two. Edit `models.json` (or pass `--models FILE`) to
change the plan. `--model ID` (or `OPENAI_MODEL`) pins every role to one model
for endpoints that do not serve these ids, and `--effort LEVEL` pins the
effort (`--effort default` sends none). Each agent's choice is logged as it
starts.

From a checkout, build the library and the Node addon once:

```sh
make lib-shared
TNY_ROOT="$PWD" npm --prefix sdk/typescript run build
npm --prefix examples/sdk/typescript install       # links the SDK from this checkout
```

Python (3.10+, needs `cffi`):

```sh
export PYTHONPATH="$PWD/sdk/python/src" TNY_LIBRARY_PATH="$PWD/build/lib/libtny.1.dylib"  # libtny.so.1 on Linux
python3 examples/sdk/python/auto_research.py "How does session isolation work?" --workspace .
python3 examples/sdk/python/codegen.py @spec.md --workspace /tmp/greeter \
    --verify "python3 -m pytest -q"
```

TypeScript (Node 24+, which runs `.ts` files directly; no build step):

```sh
node examples/sdk/typescript/auto_research.ts "How does session isolation work?" --workspace .
node examples/sdk/typescript/codegen.ts @spec.md --workspace /tmp/greeter \
    --verify "python3 -m pytest -q"
npm --prefix examples/sdk/typescript run typecheck
```

Every script has `--help`. Progress goes to stderr; the report (research) or
review (codegen) goes to stdout. `codegen` exits 0 only when `--verify` passed.

### Without a key

[`offline_provider.py`](offline_provider.py) is a scripted stand-in for the
model. The native runtime, tools, permissions and scheduler all run for real;
only the replies are canned (including a deliberately malformed reply and a
deliberately buggy generated file, so the re-ask and repair paths run). Put
`[stubborn]` in a codegen specification to make the first fix fail and watch
the escalation. Set `OFFLINE_REQUEST_LOG=FILE` on the provider to record the
role, model and effort of every request.

```sh
python3 examples/sdk/offline_provider.py 8787 &
export OPENAI_BASE_URL=http://127.0.0.1:8787/v1 OPENAI_API_KEY=offline OPENAI_WIRE_API=chat
python3 examples/sdk/python/codegen.py "A greeting CLI" --workspace /tmp/greeter \
    --verify "python3 main.py Ada | grep -q 'Hello, Ada!'"
```

`make test-sdk-examples` runs all four scripts this way and checks the
outcomes.

## What each piece shows

| You want to… | Look at |
| --- | --- |
| Configure a runtime per agent role (custom or built-in task preset, permission mode, model, reasoning effort) | `Roles` in `_common.py` / `common.ts` |
| Mix models by role and escalate to a stronger one after a failure | `Roles.selection`, `models.json`, the `tier` argument in codegen's fix loop |
| Run one turn and collect the text, handling permissions and errors | `Agent.turn`, `ask` |
| Get structured output and let the model repair a bad reply on the **same session** | `ask_json` / `askJson` |
| Fan out parallel agents and keep going when a branch fails | `investigate` in auto-research |
| Build a DAG at run time, pass upstream output downstream, override the runtime per task | `build_workflow` / `buildWorkflow` in codegen |
| Decide permissions per task instead of per run | `permission_policy` / `permissionPolicy` |
| Watch tool calls as they happen | `log_event` / `logEvent` |
| Account for tokens across sessions and workflows | `Ledger` |
| Carry memory between runs | `Lessons` |

Things worth knowing before you write your own:

- **A `Workflow` graph is fixed before it runs.** Loops and graphs that depend
  on model output are host code: run a workflow, read the result, build the
  next one. Both examples are structured that way.
- **A failed task blocks its descendants**, and the rest of the graph
  continues. Auto-research therefore fans out in one workflow and synthesises
  afterwards from whatever succeeded; codegen wants the blocking behaviour.
- **Task output is all the text the agent streamed**, not a return value. Ask
  for a fixed reply format, and for JSON ask for one fenced block at the end.
- **Python gives you `bytes`, TypeScript gives you strings**; token counts are
  `int` and `bigint` respectively.
- **The SDK default permission mode is `ask`, and workflows deny unanswered
  requests.** `auto` allows in-workspace edits, read-style shell and web tools
  natively; the policy callback only sees what is left.
- **Pass a `state_dir`/`stateDir` outside the workspace if agents write
  files.** See the comment on the scratch state directory in the helpers.
- **Dependency output and recalled lessons are untrusted model text.** The
  prompts label them as context, and verification is a host command the agents
  do not control.

## Safety

`codegen` generators and the fixer may write files and run shell commands
inside `--workspace` (default `./codegen-out`). Point it at a scratch
directory or a dedicated git worktree, not at a tree you care about.
`--verify` runs through your shell with your privileges; it is your command,
never one proposed by a model. Auto-research agents are read-only.

wasm: not applicable — these use the native Python and Node SDKs, which have
no browser build ([docs/sdks.md](../../docs/sdks.md)).
