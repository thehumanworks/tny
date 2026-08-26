# 05b — wasm behavior: clean error + CI

Parallel with 05a. Depends on 02b (flag exists). Small.

Per ADR-0017 every new capability states its wasm behavior and CI enforces
it. There is no `fork(2)` in the wasm build, so `-B` is a clean error.

## Work

- Guard the detach path so the wasm build compiles without `fork`/`setsid`
  (the platform seam rule: prefer a small `tny_can_background()` /
  `tny_detach()` in the existing host-OS seam over a fourth `#ifdef` site —
  check where `src/net/net_wasm.c` peers live before adding one).
- Error text: `tny: --background is not available in the browser build`,
  exit 1, printed before any backend work.
- Add the assertion to the wasm CI job (alongside `test_openai.py` /
  `test_codex_attach.sh` with `TNY=build/wasm/tny`).
- State the wasm behavior in the docs page that 06 touches (`docs/cli.md`
  ask section): works natively / clean error on wasm.

## Acceptance

- `make wasm` compiles; wasm CI test proves the clean error; native build
  unaffected.
