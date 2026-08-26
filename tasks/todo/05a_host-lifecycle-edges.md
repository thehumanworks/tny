# 05a — Backend host lifecycle edges

Parallel with 05b. Depends on 03 and 01 (decision 6).

## Work

- **Registry write skip**: when the background child spawns a codex host,
  do not publish it to `~/.tny/codex-host.json`
  (`cx_registry_write` call site, `src/backends/codex/codex.c:152`) — an
  invisible background process must not become the attach target for
  foreground one-shots/TUIs. Plumb a "no registry" bit through ctx or
  backend opts; keep the child's own cleanup (`codex_proc.c`) working when
  it never wrote an entry.
- **Attach death**: a background child *attached* to a foreign host (env
  `TNY_CODEX_WS` or registry) loses it if the owning TUI exits mid-run
  (ADR-0004: attached hosts are never ours). Verify the resulting engine
  error propagates to the 03 finalize path so the session ends
  `status:"error"` with the error text in `task.log` / session `error`
  field — not a silent hang. Add a poll/read-EOF timeout if the current
  code can block forever on a dead socket.
- Cursor bridge + ACP: confirm the same "child owns what it spawned,
  destroys on exit" holds; no registry equivalent exists there, so this
  should be audit-only. Note findings in this file.

## Acceptance

- Integration (python stub host, as in ADR-0004 benches): background run
  that spawns a host leaves no registry entry and no orphan process;
  killing an attached host mid-run finalizes the session as `error`.
