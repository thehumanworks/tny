# Python code-mode fixture migration

Date: 2026-09-27
Branch: `task/python-code-mode-fixtures-20260927` (parent `fb6df26`)

Scope: `run_code` cells that test fixtures send to the production runtime,
converted from Lua to standard Python using the prebound, synchronous
`tools.call(name, json_arguments)`, `tools.list()`, `tools.describe(name)`,
`json.loads`/`json.dumps` and `print`. No cell imports a module or opens a
file except where the test proves that doing so is refused. `src/`,
`tests/test_code_runtime.c`, benchmark corpora/data/scripts and historical
Lua evidence are unchanged.

The production runtime on this branch is still Lua. None of the integration
tests or the C unit suite listed below were run against a Python runtime here;
they are deferred to the runtime branch (see the last section).

## Changed fixture families

| Family | Files | Lua → Python |
|---|---|---|
| Mock-provider wrapper | `tests/integration/code_mode_fixture.py` | `lua_string` (long brackets `[==[…]==]`) → `python_string` (`json.dumps(value, ensure_ascii=False)`). Every escape it emits is also a Python escape, non-ASCII stays verbatim, NUL is escaped (run_code rejects raw NUL). `code_call` still emits `print(tools.call(NAME, ARGS))` with `timeout_ms: 30000`; `code_chat_frames`/`code_response_events` are unchanged. Covers every mock using the wrapper (`mock_openai.py`, `fake_acp_agent.py` and their callers). |
| Wrapper importers | `test_execution_command.py`, `test_mcp_http.py`, `test_background_agents.py`, `test_execution_code_mode.py` | Import/callers renamed to `python_string`; the surrounding cells were already valid Python. |
| Code-mode schema/effect | `test_execution_code_mode.py::test_both_wires_exact_schema_and_real_file_effect` | `string.find(catalog, "read_file")` → `"read_file" in catalog`; same for `describe("write_file")`/`"content"`. Schema assertions unchanged. |
| Fresh state per cell | `…::test_each_cell_has_fresh_python_state` (was `…_lua_state`) | `assert(previous_cell == nil)` → `try: previous_cell` / `except NameError: print("fresh state")` / `else: raise AssertionError(...)`. `globals()` is not used because a restricted runtime need not provide it. |
| Host-authority boundary | `…::test_restricted_python_and_json` (was `test_restricted_lua_and_json`) | Lua asserted `io/os/package/debug/require/dofile/loadfile/load` were nil. Python equivalent attempts host access instead of probing names: `open(<abs workspace>/escape.txt, "w")`, `__import__("os").open(..., O_CREAT)` and `import subprocess; subprocess.run(["touch", ...])`, each in `try/except Exception`, then `print("blocked=3")` only if all three raised. The test additionally asserts `escape.txt` does not exist. JSON half: `json.loads('{"value":42}')["value"] + 1` → `json.dumps({"answer": …})`, same whitespace-insensitive assertion. |
| Errors and bounds | `…::test_runtime_errors_and_bounds` | Syntax error `local =` → `value =`; `while true do end` → `while True:\n    pass` (40 ms); output bound `string.rep("x", 70000)` → `"x" * 70000`; memory bound → `t = []` / `while True: t.append("x" * 100000)`; call bound `for i=1,65` → `for _ in range(65)` (still 65 calls); `timeout_ms: 30001` unchanged. Assertions (`"error"` in lowered output, < 8 s) unchanged. |
| Owner stop / no replay | `…::test_owner_stop_interrupts_python_and_retains_completed_effect` (was `…_lua_…`) | `tools.call(...); while true do end` → `tools.call(...)\nwhile True:\n    pass` (a compound statement cannot follow `;`). Other private-cell sequences were already valid Python (`;` between simple statements). |
| Image catalog filter | `test_image_service.py` (`catalog_only` stream), `test_image_workflow.py::image_catalog` | Lua `ipairs`/table append/`json.encode` → `for entry in json.loads(tools.list())`, `entry["function"]["name"]`, `selected.append(entry)`, `print(json.dumps(selected))`. An empty selection now encodes as `[]`. |
| Nested web search | `test_native_search.py` | `assert(string.find(describe, "web_search", 1, true))` (plain find) → `assert "web_search" in tools.describe("web_search")`; `json.encode({query="fixture"})` → `json.dumps({"query": "fixture"})`. |
| ACP bridge budget | `tests/fixtures/acp_bridge_deadline.c` | `"while true do end"` → `"while True:\n    pass"`. The two `print(tools.call('…', '…'))` cells were already valid Python. |
| C preview wire wrapper | `tests/test_openai.c::pv_code_reply` | `print(tools.call([=[%s]=], [=[%s]=]))` → `print(tools.call(` + `jescape(name)` + `, ` + `jescape(arguments)` + `))`. `jescape` emits `\" \\ \n \r \t \u00XX` and raw UTF-8, all valid in a Python string literal, so it matches `python_string`. |

Test methods renamed: `test_each_cell_has_fresh_lua_state`,
`test_restricted_lua_and_json`,
`test_owner_stop_interrupts_lua_and_retains_completed_effect` → `python`
variants. No Makefile, Nix file, inventory or other test references them
(checked with `grep -rn`).

## Deliberately unchanged

- Cells that were already valid Python with the same meaning:
  `test_execution_permissions.py`, `test_execution_library.py`,
  `test_job_artifacts.py`, `test_purposeful_swarm.py`, `test_jobs.py`,
  `test_bench_tools.py`, `test_core.c` (`print(1)` validation inputs) and
  `test_openai.c` (`print(1)`, `print(2)`, `print('first-result')`, …).
- `test_openai.c` `native_pending_lifecycle_and_allocation_sweeps`:
  `print(tools.call('native_pending', {}))` passes a non-string container in
  both languages, and the test asserts the cell is refused before it runs
  (`f.invokes == 0`). It is valid Python as written.
- Comments naming Lua outside cells: `tests/fixtures/acp_bridge_deadline.c:2`
  ("requested Lua execution budget") and `tests/integration/test_acp_client.py:552`
  ("Lua executed successfully"). `tests/bench/bench_tools.py:18` is benchmark
  prose. Left for the runtime branch to reword with the product text.
- `src/core/tools.c` `run_code` description and `tools_code_instructions()`
  still say Lua and show `json.encode({path=…})`; that is product text owned by
  the runtime branch.

## Checks run on this branch

Host: CPython 3.14.7 (`~/.local/share/mise/installs/python/3.14.7`), Ruff 0.16.6
and clang-format 23.1.0 (the `.mise.toml` pins), `TMPDIR=/var/tmp/…`.

- `python -m py_compile` on all eight changed Python files: pass.
- `ruff format --check` and `ruff check` on the changed Python files: pass.
- `clang-format --dry-run -Werror` on `tests/test_openai.c` and
  `tests/fixtures/acp_bridge_deadline.c`: pass. `cc -std=c11 -fsyntax-only
  -Wall -Wextra` on both: pass.
- A throwaway checker (kept outside Git under `.agent/check_cells.py`) walked
  the AST of the changed and already-Python cell files, evaluated f-string and
  concatenated cells with placeholder values, and compiled 69 cells with
  CPython `compile(..., "exec")` without executing them. Only the intentional
  `value =` syntax-error cell fails to compile, as expected.
- Compile alone does not detect a leftover Lua long string: `[[x]]` is a
  valid Python list. The checker therefore also parses every `code_call`
  output for adversarial names/arguments (quotes, backslashes, `]]`, `]=]`,
  newline, tab, NUL/control bytes, é, emoji, U+2028) and asserts the AST is
  `print(tools.call(<str>, <str>))` with both constants equal to the inputs and
  `timeout_ms == 30000`; the same for chat-frame and Responses event wrapping
  and for a Python emulation of `pv_code_reply`/`jescape`.
- Effect-free cells were executed against an in-memory `tools` stub (no tny,
  provider, network or file effect): the image filter selects exactly
  `image_generate`/`image_edit` and encodes an empty selection as `[]`; the
  fresh-state cell prints `fresh state` on empty globals and raises when
  `previous_cell` is preset; the bound loop makes exactly 65 calls; the
  nested search passes `{"query": "fixture"}` as a string; the JSON cell
  prints `"answer":43`. The host-access, infinite-loop and memory cells were
  compiled only.
- The old Lua cells (`while true do end`, `local t = {}`) are rejected by the
  same compile step, and the old `[[…]]` wrapper output is rejected by the AST
  assertion.

No provider, model or network call was made. No tny binary was built or run.

## Deferred to the runtime branch

These need the Python production runtime and were not run here:

1. `make test` (C unit suite: `test_openai.c` preview/wire cases) and the
   integration files above: `test_execution_code_mode.py`,
   `test_execution_command.py`, `test_execution_permissions.py`,
   `test_execution_library.py`, `test_mcp_http.py`, `test_background_agents.py`,
   `test_image_service.py`, `test_image_workflow.py`, `test_native_search.py`,
   `test_job_artifacts.py`, `test_purposeful_swarm.py`, `test_jobs.py`,
   `test_bench_tools.py`, `test_acp_bridge_deadline.py`, and every test driven
   by `mock_openai.py`/`fake_acp_agent.py` through `code_call`.
2. `test_restricted_python_and_json` asserts `blocked=3`: each attempt must
   raise inside the cell. If the runtime confines by process/OS policy and lets
   one attempt succeed harmlessly (for example `import subprocess` succeeds but
   `run` raises), the counter still holds; if an attempt returns normally
   without an effect, the marker (not the invariant) may need rewording. The
   invariant is that `<workspace>/escape.txt` never exists and only nested tools
   reach the workspace. The cell also needs the `str` builtin for its marker.
3. The memory-bound cell relies on `"x" * 100000` allocating a fresh string per
   iteration, as Lua `string.rep` did. CPython does not constant-fold string
   products longer than 4096 characters; a runtime that folds it would reach the
   5000 ms default timeout instead (still `error`, still under 8 s, but not the
   memory path).
4. Error-text assertions are substring checks on stable host diagnostics
   (`error`, `timeout`, `permission`, `cancelled by owner policy`,
   `no direct fallback`, `error:`), not Lua traceback formats; the Python
   runtime must keep those host strings. Python exception class names (for
   example `SyntaxError`) also satisfy the lowered `error` check.
5. `test_code_runtime.c` is owned by the runtime branch and was not touched.

Python `json.dumps` separates with `", "`/`": "` where Lua `json.encode` was
compact. No converted cell's JSON is compared as a raw string: nested
`web_search` arguments are parsed by tny (the test asserts the
`configured:fixture` result), image catalogs are `json.loads`-ed, and the
`"answer":43` check strips spaces.
