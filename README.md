# tny

A tiny, fast coding-agent harness: interactive TUI plus a noninteractive CLI.

tny is designed to beat [vercel-labs/fx](https://github.com/vercel-labs/fx) on binary size and startup while keeping that Unix-shell workflow, and to drive four backends:

- **Cursor** through the [Cursor SDK Bridge](https://cursor.com/docs/sdk/bridge) (`sdk.v1` Connect)
- **Codex** through `codex app-server` over WebSockets
- **Other agents** through [ACP](https://agentclientprotocol.com/)
- **OpenAI-compatible** HTTP providers (native tool loop)

Language: **C11**. This repository currently holds research, architecture, and agent instructions only. Implementation has not started.

Read the research pack: [docs/README.md](docs/README.md).
