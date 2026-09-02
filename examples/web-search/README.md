# Web search: token-efficient provider setup

`web_search` is advertised to the model only when a provider is configured
([ADR 0055](../../docs/adr/0055-web-search-gating-and-command-provider.md)).
The simplest provider dumps a results page into the model's context, which
costs thousands of tokens per search and still leaves the agent guessing which
hit to open. The setup below spends almost nothing on the search call itself
and lets the agent read only the pages it needs.

## `brave-search`

`examples/web-search/brave-search` is a script
with this contract:

- **Input**: `$1` is the percent-encoded query exactly as tny substitutes it
  for `{{query}}` (only `A-Za-z0-9-_.%`, a single safe shell word).
- **Fetch**: loads the Brave results page for the query with
  [lightpanda](https://lightpanda.io), a headless browser, and then fetches
  each result page the same way.
- **Save**: writes each result page as markdown to
  `/tmp/tny-search/<query>/NN-host.md`, where `NN` is the result rank and
  `host` is the page's hostname.
- **Output**: prints a short index on stdout: one line per result with the
  rank, title, URL, and the saved file path, followed by an instruction
  telling the agent to open the relevant files with `read_file`.

Because the script runs through the `terminal` tool path, the index is what
the model sees as the tool result. The full pages stay on disk until the agent
asks for them, so a search that yields ten hits costs a few hundred tokens
rather than the whole rendered pages.

## Settings

Put the script on `PATH` (or use an absolute path) and add to
`~/.tny/settings.json`:

```json
{
  "web_search_command": "brave-search {{query}}"
}
```

`web_search_command` wins over `web_search_url` when both are set. It is not
available in wasm builds, where only `web_search_url` works. See
[docs/features/mcp-and-skills.md](../../docs/features/mcp-and-skills.md#web-search-providers)
for the full provider table.
