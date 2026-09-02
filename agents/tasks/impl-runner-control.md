tny: --provider codex --model gpt-5.6-sol --effort xhigh
cwd: /tmp/wt-tny-runner-control

# Task: runner control channel for ask-user and image attach (issue #98)

Worktree: /tmp/wt-tny-runner-control (branch agent/runner-control)

## Ground rules (every agent)
- You work ONLY in the worktree named above (branch agent/<name>, cut from feat/shell-first at 5dd6f27). Never touch /Users/tomas/projects/tny or other worktrees except to READ docs/adr/0057-shell-first-native-loop.md at /Users/tomas/projects/tny/docs/adr/0057-shell-first-native-loop.md (the umbrella decision; cite it as ADR 0057).
- Read AGENTS.md (CLAUDE.md), docs/product.md, docs/architecture.md, docs/cli.md and the ADRs your task names BEFORE writing C. C11 only, no new dependencies, no ncurses, stripped binary < 1.0 MiB.
- Match the existing style exactly (see neighbouring files); run `make format` before finishing. Quality gate: `make quality CLANG_FORMAT="uvx clang-format@21.1.2" CLANG_TIDY="uvx clang-tidy@22.1.8" RUFF="uvx ruff@0.14.0"` (clang tools are not installed natively on this Mac; uvx is). Fix what it reports in files you touched.
- Tests are mandatory: unit tests in tests/test_*.c (greatest) for new C, integration fixtures under tests/integration/ where the change crosses a process or wire boundary, and add your new functions to TARGETS in tests/mutation/mutate.py if the unit suite might cover them only nominally. `make test` must pass (it builds with ASan/UBSan). For new allocation-heavy paths run the relevant test binary under `leaks --atExit -- <binary>` (macOS; valgrind is unsupported on arm64 Macs) and fix reported leaks.
- Documentation is part of the change: update docs/ (cli.md, features/, backends/ as relevant) and WRITE the ADR assigned to you in docs/adr/NNNN-<slug>.md using the existing ADR format (Context / Decision / Consequences, Date, Status). State the wasm behaviour (works / remote-only / clean error) of anything new, per AGENTS.md.
- If you add a make target, test fixture directory, or a tool the suite shells out to, update nix/source.nix and nix/tests.nix in the same change. Do NOT run nix.
- Do not commit secrets. Do not push. When done: `git add -A && git commit` on your branch with a conventional message that names the issue (e.g. "feat(cli): add tny edit (#96)"), then print a summary: files changed, tests added, how you verified, open questions.
- Keep to your scope; other agents are concurrently editing Makefile SRC lists, src/main.c, src/cli/, docs/cli.md, so keep edits to shared files minimal and additive to ease merging.

## Spec (from issue #98 and ADR 0057)
Add a typed control channel on the session socket so a subprocess started by the `terminal` tool can ask the human a free-text question or attach an image to the next request.
Read first: docs/adr/0053-forked-turn-isolation.md, docs/adr/0008-native-loop-images.md, src/core/runner.c (rn_accept ~434, client ops ~708, sock path fallback :258), src/core/tools_shell.c (:66-142), src/core/tools_ext.c (ask_user_question :332-342, read_image :304-331), src/core/tools.c (pending images ~510), tests/test_runner.c, src/backends/acp/acp_turn.c (~192, bounded nested pump precedent).
Problems to solve (verified in review):
1. Deadlock: `terminal` blocks in its own poll loop while the runner accepts socket clients only in its outer loop. A child that connects and waits is never serviced. The terminal wait loop must pump ONLY control ops and owner replies while a child runs; never re-enter backend dispatch.
2. No client roles: every connected client can send turn/cancel/perm/end. Add a handshake with roles owner | observer | tool, a per-role op allowlist, and correlation ids.
3. Image timing: an attach request must be queued through the existing pending-image path (ADR 0008) before the batch completes so it rides the next provider POST.
4. Not universal: `tny acp` server mode, --ephemeral, wasm, and the macOS in-process fallback have no runner socket (ADR 0053 §9). Those modes need a frontend-callback adapter or a clean unsupported error; document which.
Design:
- `terminal` exports the RESOLVED socket path as TNY_SESSION_SOCK (deep paths have a fallback) plus TNY_SESSION_ID into the child env.
- Ops (NDJSON, same framing as today): {"op":"ask_user","id":..,"question":..} → forwarded only to the owning frontend; observers cannot answer; reply {"id":..,"answer":..} or error. {"op":"image_attach","id":..,"path":..} → validate (path inside allowed roots, magic bytes png/jpeg/gif/webp) → ACK or error. On owner disconnect, questions fail closed.
- New CLI verbs: `tny ask-user QUESTION` (question on argv or stdin) and `tny image attach PATH`. With no TNY_SESSION_SOCK: one stderr line `tny: no session socket (set TNY_SESSION_SOCK or run inside tny)` and exit 1; never read /dev/tty. `--json` output objects carry a `kind` field. Exit codes 0/1/2/130.
- ask_user returns arbitrary text, replacing today's yes/no via the permission hook; keep the existing non-interactive fallback string for callers that cannot answer.
- TUI: render the question and deliver the answer over the socket (the TUI is the owner client of the runner). `tny ask` (non-interactive) answers with the fallback.
- `tny acp` server: map ask_user to the ACP client question/permission callback, not the socket (in-process).
Tests: tests/test_runner.c bidirectional split-boundary framing, role enforcement, correlation, disconnect-fails-closed, and a no-deadlock test with a child blocked on ask_user during a `terminal` call. Integration: a script run through `terminal` that calls `tny image attach` and the next mock request carries the image part (tests/integration/mock_openai.py). Mutation TARGETS.
Docs: docs/cli.md (both verbs), docs/features/sessions.md or the socket doc, ADR 0053 amendment note, and ADR 0058 "session control channel: roles and tool ops". wasm: clean error (no socket).
