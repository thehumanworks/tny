# 0195 — Retire unused desktop interfaces

Date: 2026-10-19
Status: accepted

## Goal

Keep one agent harness instead of maintaining unused desktop products.

## Decision and dependency boundary

Remove the Rust/Slint companion (`gui/`), its Cargo dependency closure, Lean
GUI models and dedicated workflow. Remove the independent `tnytty/` terminal
emulator, its broker, renderer, tests, delegated Make targets, Nix benchmark
inputs and native CI steps. Remove their active documentation and generated
site pages. Historical ADRs and dated verification records remain historical,
not promises that these products still ship.

`tnytty` does have durable terminal panes, but those belong to its own broker.
The harness does not import, link or execute it: `src/` and `include/` contain
no tnytty references, and the Make targets were delegation only. Harness
sessions are owned by `src/core/runner.cpp` and persisted by
`src/core/session.c`; the CLI/TUI reconnect to the runner socket. Those modules,
the public ABI, session tests, SDKs and wasm landing terminal stay unchanged.

This supersedes ADR 0045's sibling terminal product and ADR 0166's experimental
Slint companion, not ADR 0166's separate global-session decision.

## Success measure

No GUI/emulator build or CI entrypoints remain. The harness builds without
those trees, and existing isolation tests still demonstrate client-disconnect
survival and session resumption. Existing site tests cover the retained wasm
terminal. Release/package contracts remain the same.

There is no new replacement abstraction, UI or session implementation.
