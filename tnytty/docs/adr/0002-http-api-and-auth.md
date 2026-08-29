# 0002 — The HTTP API is the scripting surface; non-loopback binds require a bearer token

Date: 2026-08-29
Status: accepted

## Context

"Scriptable" could mean a control socket, a wire protocol, or embedding.
tnytty's users are scripts, agents, and remote collaborators; the lowest
common denominator they all speak is HTTP + JSON. But a terminal session
is shell access: exposing it publicly without auth is handing out a
remote shell.

## Decision

- One REST surface ([http-api.md](../http-api.md)) served from the same
  event loop as the ptys. No second protocol; future streaming is SSE on
  the same server (phase 3).
- Default bind is `127.0.0.1:7681`, no token required — parity with how
  local tools trust local users.
- A non-loopback bind **refuses to start without a token**: `--token` /
  `TNYTTY_TOKEN`, else tnytty generates 32 hex chars from the OS RNG and
  prints them once to stderr. Every request then needs
  `Authorization: Bearer <token>`, compared in constant time.
- Session ids are short random hex (OS RNG), not sequential, because
  they travel in shared URLs.
- Session sharing is URL + token; finer-grained read-only tokens are
  phase 3, not phase 1 scope creep.

## Consequences

- `curl` is the reference client; anything that can HTTP can drive a
  terminal, which is the product point.
- A leaked token is shell access — documented in bold; rotating is
  restart-with-new-token in phase 1.
- No TLS in-process: public deployment goes behind a reverse proxy
  (documented), keeping TLS out of the binary and the size budget.
