# 0062 — Native tool profiles: advertise and enforce

Date: 2026-09-02
Status: accepted (implements the opt-in profile in issue #100 and ADR 0057)

## Context

The native OpenAI-compatible loop has a broad structured tool vocabulary. ADR
0057 keeps every implementation while making a smaller shell-first vocabulary
available for measurement. Hiding schemas alone is insufficient: a provider
can replay an old call or hallucinate a hidden name, and dispatching it would
make the advertised authority boundary false.

Terminal output also needs a shell-friendly contract. The existing `all`
profile uses a bounded preview and `read_tool_result` session handle, but an
agent operating primarily through `terminal` needs the exit status, byte
count, cwd, and a complete spill file without first consuming a second tool.

## Decision

1. `tools` in user `~/.tny/settings.json`, overridden by `TNY_TOOLS`, accepts
   `all`, `terminal+edit`, or `terminal`. `all` remains the default.
2. The native loop advertises and accepts these built-ins:

   | Profile | Built-ins |
   | --- | --- |
   | `all` | The existing complete schema |
   | `terminal+edit` | `terminal`, `edit_file`, `read_image`, plus `ask_user_question` when the session has an interactive question callback |
   | `terminal` | `terminal`, `read_image` |

   `read_image` is the deliberate exception to terminal-only purity because
   image pixels cannot travel through stdout (ADR 0008). Registered libtny
   custom tools are appended and callable in every profile.
3. The same allowlist gates validation. An unadvertised built-in fails with
   the ordinary `unknown tool NAME` result and never reaches permission lookup
   or dispatch. Shell profiles hide all MCP meta-tools because the prompt
   directs the model to `tny mcp call SERVER/TOOL`. The current native
   `mcp_select_tool` has no separate SDK/replay-only capability boundary, so a
   direct or replayed call is rejected like every other unadvertised built-in.
4. Only the native CLI/TUI OpenAI-compatible loop uses the opt-in profiles.
   Host backends receive neither tny's schemas nor its shell prompt. libtny,
   wasm, and `tny acp` keep `all`; an explicitly requested profile produces
   one status line when wasm or ACP server mode ignores it. libtny's explicit
   context reads no ambient settings or environment, so it is always `all`.
   The existing wasm terminal implementation remains a clean unsupported
   error; the profile does not create a miniature shell in the browser.
5. Shell profiles append one stable, cache-friendly system-prompt block. It
   states that each command starts in the workspace cwd and cwd resets between
   calls; dependent commands use `&&`; inspection uses narrow `rg -n` and
   `sed -n`; mutation uses `tny edit FILE` (or `edit_file` in
   `terminal+edit`) and never `sed -i`; callers read `exit:`; MCP arguments
   use JSON on stdin; image/question socket verbs may report `no session
   socket`; and subagents use background `tny ask -B --json` followed by
   `tny session ID --wait --json`.
6. A foreground `terminal` call in a shell profile returns `exit: N`,
   `bytes: N`, and `cwd: PATH`, followed by at most
   `min(max_tool_result_bytes, 8 KiB)` output bytes. If truncated, `full: PATH`
   names a `0600` file under `<session>/results/`, or `~/.tny/results/` when
   there is no session. Output is streamed to that file from byte zero while
   the child runs, rather than first passing through the old 512 KiB buffer.
   A 64 MiB hard cap terminates a command that exceeds it. `all` retains its
   existing result text and `read_tool_result` handle behavior unchanged.
7. `tny status` and `tny doctor`, including JSON output, report the effective
   profile.

## Consequences

- The A/B in issue #103 can change prompt vocabulary without deleting or
  forking tool implementations.
- Advertisement and execution cannot drift for built-ins; old/replayed calls
  fail closed under the active profile.
- Shell-first turns get deterministic process facts and lossless ordinary
  output spills, at the cost of one temporary result file while each
  foreground command is collected. Untruncated temporary files are removed.
- MCP schemas no longer consume the shell-profile context, while the same MCP
  permission identity remains available through the CLI verb specified by
  ADR 0057 and ADR 0064.

Amendment (2026-09-02, after the #103 pilot): 9 of the `terminal` arm's 10
repair loops were a first `tny edit` call with an invented calling convention
(`--old`/`--new` flags, two FILE arguments, or a bare heredoc without the
`*** SEARCH` fence). The prompt block therefore carries the fence form
verbatim — `printf '*** SEARCH\nOLD\n*** REPLACE\nNEW\n*** END\n' | tny edit
FILE` — and says "no --old/--new flags, one FILE". The block stays stable
across turns, so the cache-friendliness argument is unchanged.
