# Prompt optimisation

Type or dictate a draft, then press **Ctrl-O**, or enter:

```text
/optimise fix the parser error
/optimise --model inception/mercury-2.5 fix the parser error
/optimise --provider openai --model YOUR_MODEL fix the parser error
```

The optimiser rewrites the prompt in the composer. Edit it, then press Enter
to send it through the conversation's selected provider and model. Dictation
works normally: Ctrl-R, speak, Enter to transcribe, Ctrl-O to optimise, then
Enter to submit. `/transcript` displays the conversation transcript.

Optimisation requires an idle composer. While it runs, Esc, Ctrl-C, or Ctrl-D
cancels it; typed/pasted text and Enter do not submit or modify the draft.
Cancellation, provider errors, empty or invalid output, and limit exhaustion
leave the original prompt intact. `/optimise` removes the command/options
from the draft before starting. With Ctrl-O the caret is preserved on failure.
Images already attached remain attached to the eventual conversation turn;
the optimiser receives the text draft and project context, without image bytes.

## Model configuration

The default is **OpenRouter**, model **`inception/mercury-2.5`**. Set
`OPENROUTER_API_KEY` in the environment. The default endpoint is
`https://openrouter.ai/api/v1`, using Chat Completions and tool calling.
An existing `openrouter` provider profile or `OPENROUTER_BASE_URL` overrides
the endpoint; its credentials and explicit wire setting are respected.

Configure independent defaults in `~/.tny/settings.json`:

```json
{
  "optimise": {
    "provider": "openrouter",
    "model": "inception/mercury-2.5"
  }
}
```

For each field, precedence is explicit optimisation option, then
`TNY_OPTIMISE_PROVIDER` / `TNY_OPTIMISE_MODEL`, then `optimise.provider` /
`optimise.model`, then the built-in default. Ordinary conversation model,
effort, fast-tier, and task settings do not change the optimiser. Selecting
another provider usually also requires selecting a model that it serves.
Native OpenAI-compatible providers, including subscription profiles, work;
Cursor and ACP hosts are refused because tny cannot enforce their tool set.

## Project exploration

The optimiser can list directories, glob paths, search contents, read files,
inspect file metadata, and page through large tool results. It is instructed
to explore relevant subdirectories and project instructions when the draft
depends on them, preserve the user's scope, and use verified file paths and
constraints. It skips project exploration for unrelated prompts.

The tool schema and executor enforce a read-only tool set. Shell commands,
writes, MCP, extensions, and delegation are unavailable. Exploration uses
the current workspace and additional directories, or the attached SSH tool
runtime. Reads needing an approval are denied. Project content is reference
material for the rewrite, and cannot authorise executing the draft's task.

The service uses a separate ephemeral native session and the existing event
loop. It does not save an optimisation transcript or change the conversation's
history, task, provider, model, or defaults. Drafts/results are UTF-8 text of
at most 64 KiB; control sequences and whitespace-only results are rejected.
Exploration has a 12-step cap (a smaller project/active limit wins), a
120-second deadline checked between engine steps, and at most 16 KiB per
tool result (a smaller project limit wins).

## CLI and wasm

```sh
tny optimise 'fix the parser error'
tny optimise --model inception/mercury-2.5 --json 'fix the parser error'
tny dictate --seconds 10 | tny optimise --stdin
```

`--provider` and `--model` before or after `optimise` select the optimiser.
`--cwd` selects the project. Piped input is read automatically; `--stdin`
makes it explicit. `--` treats remaining arguments as literal prompt text.
Stdout contains only the improved prompt, or JSON with `kind`, `provider`,
`model`, and `text`; stderr carries progress/errors. Nothing is submitted to
the conversation agent. Exit codes: 0 success, 1 failure, 130 cancellation.

The same C service works in wasm through the existing HTTP and polling
seams. It can explore files visible to that wasm filesystem. The browser
cannot read the host project unless files have been made available there;
provider access still requires network/CORS support. Native CLI/PTY and wasm
CLI fixtures run in CI without live credentials.

See [ADR 0082](adr/0082-prompt-optimisation.md). The
[OpenRouter model catalogue](https://openrouter.ai/api/v1/models) was checked
on 2026-09-09: `inception/mercury-2.5` advertises `tools` and `tool_choice`.
