# 0017 — tny.wasm: the real CLI in the browser, at parity by construction

Date: 2026-08-22
Status: accepted (supersedes the "no wasm build" non-goal in
[0005](0005-client-side-landing-terminal.md); 0005's key-handling rules —
BYOK, sanitize at intake, never post to GitHub — carry forward unchanged)

## Context

The landing terminal was a hand-written JS agent loop (`site/assets/term.js`)
that re-implemented the native loop's wire. It drifted from the CLI once
already: the chat-completions 400 that motivated [0016](0016-responses-api-default-wire.md)
was found in the JS loop, not in the C code. Two implementations of one
protocol cannot be kept identical by review.

## Decision

Compile the actual `tny` binary to WebAssembly and run it on the page.
**Parity is enforced by construction, not discipline:**

- One codebase, one Makefile. `make wasm` (node artifact) and
  `make wasm-web` (browser artifact) build `SRC_SHARED` — the same sources
  as `make release` minus the native transports — plus `src/net/net_wasm.c`.
- Platform differences live only at three seams that were already
  interfaces:
  1. **transport** — `src/net/net.h` is the boundary; `net_wasm.c`
     implements `http_conn` on `fetch()` + ReadableStream and `ws_conn` on
     the browser/node WebSocket, over a pseudo-fd registry. `tcp.c`,
     `stream.c`, `http1.c`, `ws.c`, wslay and picohttpparser are excluded
     from the wasm source list wholesale — no `#ifdef` riddling, `nm`
     stays honest.
  2. **blocking waits** — every poll site routes through `tny_poll()`
     (`src/util/tny_poll.h`): native forwards to `poll(2)` verbatim; wasm
     waits on the registry and yields to the JS event loop via Asyncify.
  3. **host OS** — spawn/tty/threads. Spawn paths compile against
     Emscripten's failing stubs and produce the existing "missing host"
     error shapes; the prewarm thread degrades to the lazy path
     (`pthread_create` returns EAGAIN, the code already handled that); the
     page terminal is always a tty and reads its size from
     `Module.tnyWinsize`. The page's xterm enables `convertEol` to emulate
     the native tty's retained `OPOST`/`ONLCR` flags; the wasm stdout sink
     delivers raw LF bytes and does not provide that line discipline itself.
- The wasm binary passes the **same integration suites against the same
  strict mocks** in CI on every PR (`test_openai.py`, `test_acp_ws.sh`,
  `test_codex_attach.sh`, parameterized by `$TNY`), plus a headless-browser
  smoke of the page itself. Drift is a red X.

What runs where:

| Backend | native | wasm | mechanism |
| --- | --- | --- | --- |
| openai | ✓ | ✓ | Responses/chat wires over fetch |
| codex | ✓ | ✓ attach-only (`--codex-ws`, [0004](0004-time-to-first-token.md)) | browser/node WebSocket; no spawn |
| acp | ✓ | ✓ remote-only (`--agent ws://…`) | one JSON-RPC message per text frame; implemented natively too, so it is one code path |
| cursor | ✓ | ✗ clean error | needs a spawned local bridge |

Native tools (fs, sessions, skills, permissions, compaction) run unmodified
on MEMFS in the browser and on the host filesystem (NODERAWFS) under node.
`terminal`/`open_file` return the existing tool-error shape when `fork`
fails. Sessions persist for the tab (MEMFS); OPFS is later and unpromised.

## Phase-0 spike results

- **Asyncify vs JSPI vs workers.** JSPI is Chrome-only; worker+Atomics
  needs COOP/COEP headers GitHub Pages cannot set. **Asyncify** it is,
  with broad instrumentation first. Measured cost: a minimal fetch loop
  grew 6.8 k → 17.4 k wasm (~2.5× on a toy); the full binary lands at
  ~695 k js+wasm — under half the 1.5 MiB Linux budget, so no narrowing
  was needed.
- **node as CI runtime.** An emcc fetch loop streams from
  `mock_openai.py` under node 22 unmodified; argv/stdout/exit codes behave
  with a plain `-o tny.js` script build (a `.mjs` build is a factory that
  does not self-run). One caveat that cost a day of confusion: an
  Asyncified `main`'s return value is dropped, so `main` must call
  `exit(rc)` explicitly or every failure exits 0.
- **codex Origin/auth.** Node's WebSocket sends no `Origin` header:
  `mock_codex_ws.py` (which fails any upgrade carrying one, the way the
  real app-server 403s) accepts the wasm attach, so wasm-codex ships for
  node/CI. Browser WebSockets always send `Origin` and cannot attach an
  `Authorization` header at all — browser-codex needs a real-app-server
  check and is not claimed in v1; `ws_connect` refuses a bearer on wasm
  with a clear message rather than silently dropping auth.

## The load-bearing rules in net_wasm.c

- **JS never calls into C.** Fetch/WebSocket handlers only append to
  per-fd queues and resolve the poll waker; C pulls when awake. The only
  suspension points are `tny_poll` and the header/handshake waits. This is
  the Asyncify re-entry contract, not an optimization.
- **Readiness must be consumable.** A pseudo-fd that reports ready forever
  (the first build's unconsumed-headers flag) makes `tny_poll` return
  without suspending, which starves the very event loop the fetch needs —
  a livelock with zero CPU idle. Every ready condition must clear when the
  caller consumes it.
- **Raw `poll(2)` is poison on wasm.** It returns immediately for pseudo
  fds, turning any wait loop into a spin (this wedged the first codex
  attach). Waits belong to `tny_poll`; `tny_poll(NULL, 0, ms)` is the
  portable sleep.

## Footguns confirmed the hard way

- Browser `fetch()` discards the tail of a truncated chunked body
  (`ERR_INCOMPLETE_CHUNKED_ENCODING`) instead of delivering
  bytes-then-error like sockets and node's undici do. A provider that ends
  SSE streams with an abrupt close (what `mock_openai.py` deliberately
  models) loses its terminal event in the browser. The browser smoke sets
  `MOCK_CLEAN_EOF=1`; real gateways that close abruptly will surface as
  "stream aborted" on the page and there is nothing we can do below the
  fetch API.
- Emscripten's termios/isatty stubs would demote the page terminal to
  non-tty line mode; the TUI forces `tty` when the page bootstrap owns
  stdio. xterm.js is already raw.
- The exit goodbye must be written after the binary's final erase/restore
  flush (stdout batches per microtask), or the TUI's last paint wipes it.
- `EM_JS` bodies are C token soup first: `??=` forms a trigraph under
  `-std=c11`, so spell it `||`/explicit assignment.
- fetch cannot stream request bodies cross-browser: nothing may ever
  assume chunked uploads on the wasm transport (we buffer whole POSTs).
- `https://` page → `ws://` remote is mixed content; loopback is allowed
  by current Chrome/Firefox, anything else needs `wss://`.

## Consequences

- `site/assets/term.js` is deleted. `term-core.js` survives as the intake
  and vault helper library (sanitizeApiKey, URL-hash intake, SSE parser
  unit-tested for the mocks). `test_site.py` fails if any site JS regrows
  a provider wire.
- CI gains a `wasm-node` job (emsdk 6.0.8): builds both artifacts, runs
  the openai/acp-ws/codex-attach suites with `TNY=build/wasm/tny`, guards
  the artifact size (`wasm-size-check`, budget = the 1.5 MiB Linux
  budget), and runs the Playwright page smoke.
- Pages CI builds `tny-web.mjs` and publishes it under `assets/wasm/`;
  the artifact is never committed to `site/` (gitignored), but the CI
  mirror into `docs/assets/wasm/` **is** committed — Pages deploys from
  the `main:/docs` branch, so the publish gate must stage before diffing
  (`git add docs` + `git diff --cached`) or brand-new untracked
  artifacts are silently skipped.
- AGENTS.md gains the invariant: every new backend or tool states its
  wasm behavior (works / remote-only / clean error) — review catches the
  statement, CI catches the behavior.
