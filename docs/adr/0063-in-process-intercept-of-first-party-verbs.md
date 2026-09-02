# 0063 — In-process intercept of first-party tny verbs inside `terminal`

Date: 2026-09-02
Status: accepted (implements decision 5 of [ADR 0057](0057-shell-first-native-loop.md))

## Context

ADR 0057 turns capabilities into `tny` subcommands the model reaches through
the `terminal` tool. That is the right vocabulary from another harness or a
script, where a cold `tny` process is exactly what the caller wants. Inside
tny's own turn it is a regression: a nested `tny edit` runs with no permission
engine, no session grants, no undo record, and no `--ssh` context, and a
nested `tny mcp call` cold-starts a second copy of a server this session has
already warmed and handshaked. `tny ask-user` and `tny image attach` do
survive nesting through the ADR 0058 control socket, but only by paying a
connect, a handshake, and a correlated round trip for something the turn is
holding in memory.

The tempting fix — teaching the shell child to talk back into the parent —
grows a second, weaker copy of the harness. The cheaper fix is to notice, at
the moment the tool call is prepared, that the command *is* a first-party verb
and to run the code the equivalent typed tool would have run.

## Decision

**1. Recognise, do not interpret.** `src/core/shellwords.c` splits a single
simple command with POSIX-sh quoting (single quotes, double quotes, backslash
escapes) and stops at the first unquoted character the shell would act on:
`; | & < > ( ) $ \` newline` plus the pattern characters `* ? [ { }` and a
leading `~` the shell would expand to a user's home. An unterminated quote, a
trailing backslash, an expansion inside double quotes, or more than 64 words
is a refusal. A refusal is never an error: the command goes to `/bin/sh`
unchanged, exactly as before. The non-intercepted path is byte-for-byte the
path it was.

**2. Two payload shapes, because payloads ride stdin** (ADR 0064). A here-doc
(`tny edit FILE <<'EOF' … EOF`) whose delimiter is quoted, or unquoted with a
body containing no `$`, backtick, or backslash; and a single left-hand
producer piped into the verb — `printf FORMAT` with no `%` conversion,
`echo [-n] WORD…` with no backslash, or `cat FILE` for one readable file
inside the allowed roots. `printf` and `echo` are reproduced only where every
shell agrees on the bytes (`\0nnn`, `\xhh`, `%s`, and dash's backslash-eating
`echo` are all refusals). Anything else — a second pipe, a redirection, an
env-assignment prefix, a `&&` — is not intercepted.

**3. Each verb becomes the typed tool it stands for**, before the permission
lookup, so the verdict, the rule category, the session grant, and the prompt
are identical to the structured call:

| Command in `terminal` | Route | Permission identity |
| --- | --- | --- |
| `tny edit [--json] [--marker M] FILE` | `tny_edit_file_exact` + the undo hook, or `tool_ssh_edit_exact` under `--ssh` | `edit_file` + resolved path |
| `tny mcp call SERVER/TOOL` | `mcp_call_tool_raw` on the session's warmed client | `mcp:server/tool` |
| `tny memory get\|set\|list …` | the `memory` tool | `memory` |
| `tny skill show NAME` | the `skill` tool | `skill` |
| `tny image attach PATH` | `tools_queue_image` (allowed roots only) | `read_image` |
| `tny ask-user [--json] Q` | the engine's frontend ask hook | `ask_user_question` |
| `tny ask …` without `-B` | refused | — |
| `tny ask -B …`, everything else | `/bin/sh`, unchanged | `terminal` + command |

**4. The result is the CLI's contract, not a shell transcript.** An
intercepted call returns an `exit: N` line followed by the verb's stdout and
then its stderr. `tny edit` shares its stdin grammar, its usage diagnostic,
and its rendered result with `src/cli/cmd_edit.c` through `core/edit.c`, so
the two cannot drift; the other verbs reproduce their CLI's text and exit
code. The result is bounded like any tool result (a session handle, rather
than the standalone `tny mcp call` spill file, because the model can read a
handle).

**5. A foreground nested agent is refused.** `tny ask` without `-B` would run
a second agent loop underneath this one — invisible to the frontend, to
cancellation, and to the step budget. The tool error names the alternative:
`tny ask -B "…"` plus `tny session ID --wait --json`. With `-B` the command is
*not* intercepted; it becomes a real detached child, as designed.

**6. A nested tny cannot widen authority.** Every `terminal` child is started
with `TNY_NESTED=1` and `TNY_NESTED_MODE=<effective mode>`. In a nested
process, settings-derived `permission_mode` is clamped to the parent's mode
silently, while an explicit `--permission-mode` or `TNY_PERMISSION_MODE` that
is wider fails with one line on stderr and exit 1. Width is
`ask < auto < yolo`.

**7. `--ssh` follows the file, not the command** (ADR 0022). The remote host
has no tny, so `ssh host tny …` is never emitted. An intercepted `tny edit`
under `--ssh` cats the remote file, applies the shared exact-match engine to a
local staging copy — so the match policy, the ambiguity count, and the nearest
unique context line are the same as locally — and streams the result back into
a temp file that is renamed into place. The other intercepted verbs stay local,
exactly as their typed tools do. A command that is not intercepted still runs
on the remote host, unchanged.

**8. Events name the verb.** `TOOL_START` carries the verb and its target as
its detail (`tny edit docs/x.md`, `tny mcp call deploy/status`) instead of the
`terminal` arguments blob, and a permission prompt shows that label ahead of
the typed identity it resolved to. `TOOL_END` keeps carrying the result, which
is what renderers read; the tool *name* stays `terminal` so extensions and ACP
clients keyed on it are unaffected.

**9. Not gated by profile.** The intercept is active in every tool profile and
does not depend on which tools the schema advertises: it is about what the
shell command means, not about what the model was offered.

**wasm:** not applicable in practice. `terminal` cannot fork in the browser
and returns its existing clean tool error (ADR 0017); the recogniser is
platform-free C that simply never sees a command there.

## Consequences

- `tny edit` inside a turn is now permission-checked as `edit_file`, is
  undoable with `/undo`, and works over `--ssh`. Previously it was a shell
  command matched against `bash` rules with no undo and no remote route.
- `tny mcp call` inside a turn no longer spawns a second server process; the
  session's warm client answers, and its cold-start cost disappears.
- The recogniser is a UX accelerator, never a security boundary (ADR 0057
  decision 6): a `tny` on `PATH` that is not this binary would be intercepted
  by name. Authority still comes from the permission engine and the OS
  sandbox, both of which the intercept goes *through* rather than around.
- Users lose the exact process semantics of the nested CLI for the intercepted
  shapes: no separate exit status in `$?` mid-pipeline, no signal handling of
  its own, and `--cwd`/`--provider`-style global flags before an intercepted
  verb are not recognised (only `--json`), which sends the command to the
  shell instead. This is deliberate: the boundary is documented and narrow,
  and the standalone binary still works — it just runs cold.
- `tny memory` and `tny skill show` are, for now, intercept-only: they exist
  as verbs inside a turn but have no standalone `main.c` dispatch yet. Adding
  them later must keep the two shapes identical.
- Anything the recogniser does not understand pays nothing: one bounded
  tokenisation of the command string before the fork it was going to do
  anyway.
