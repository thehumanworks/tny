# Component verification evidence

Verified 2026-09-19 on macOS arm64, on shared branch based on
`f90a6103b3b9d3e982bc88a8edd12fd0b69ddaf3`. All authored changes are under
`tnyboard/`; no commit, root edits, child agents or live inference were used.

Verified input SHA-256:
`72856a9bf7b2652859d28de8e562ac14c5479714d8e8685ed249eba3e4b43675`.
This hashes the sorted component `*.py`, `tests/*.py`, `schemas/*.json`, Makefile
and pyproject paths: each UTF-8 path, NUL, file bytes, NUL. Documentation is excluded.

## Final checks

| Command/check | Result |
| --- | --- |
| `make -C tnyboard check` | PASS: 30 unittest cases on Python 3.14.7; Ruff 0.16.6 lint and format checks pass |
| `make -C tnyboard test PYTHON=/usr/bin/python3` | PASS: same 30 cases on Python 3.9.6, no skips |
| `make -C tnyboard lint RUFF='uvx ruff@0.14.0'` | PASS: pinned repository Ruff version, lint and format |
| `uv build --wheel --out-dir tnyboard/dist tnyboard` | PASS: `tnyboard-1.0.0-py3-none-any.whl`; no runtime requirements |
| Isolated wheel smoke | PASS: `uv venv --python /usr/bin/python3 TEMP/venv`, `uv pip install --python TEMP/venv/bin/python WHEEL`; from outside the repository and without PYTHONPATH, RPC init/create, `python -m tnyboard ... show`, installed `tnyboard ... show`, and installed schemas/docs existence |
| JSON Schema Draft 2020-12 | PASS: ephemeral `uv run --no-project --with 'jsonschema==4.23.0' python` check; all four schemas pass `Draft202012Validator.check_schema`; 12 real operation request/result pairs validate with a local referencing registry |

The JSON Schema check used init, create, claim, comment, move, dispatch-intent,
uncertain dispatch-result, confirmed reconciliation, assign, configure, get and
list. `jsonschema` is verification-only; the component and its unittest suite use
only the standard library. Permanent schema/API field-drift tests are included in
`tests/test_hardening.py`.

## Covered behavior

- On-disk layout, board/ticket revision fencing, forward/backward moves, lead and
  local operator override, owner assignment/inheritance, current claim tokens,
  takeover, comment access while claimed, and occupied-column constraints.
- Process (`spawn`, four workers) and thread races: exactly one successful
  mutation with the same expected revision; claim and intent reservation races.
- Durable pending/failed/uncertain/confirmed dispatch; no duplicate pending or
  uncertain intent; board-wide immutable intent IDs; independent ticket intents;
  job linkage, explicit reconciliation, no mutation of terminal outcomes.
- Duplicate JSON keys, invalid Unicode/types/IDs, oversized input/stored files,
  unknown fields, malformed persisted history, symlink/hardlink rejection and
  case-insensitive ID collision protection.
- Injected pre-replace I/O failure, document overflow, stable lock inode and
  unchanged persisted JSON after validation/permission/revision/claim errors.
- Loopback bearer authentication, origin rejection/no CORS, malformed and
  duplicate framing headers, incomplete bodies, request timeout and split body,
  HTTP mutation/read-back and claim rejection through the shared API.
- One-request/one-result CLI, error exit status, ergonomic move, non-TTY fallback,
  line-oriented interactive inspect/move/comment/refresh/quit, control/bidi/Unicode
  escaping and narrow-width rendering.
- Python 3.9 syntax plus actual Python 3.9 execution and wheel installation.

## Corrections during verification

The first lint run reported import ordering and explicit subprocess options;
these were fixed. An expanded test run reported a late-bound test closure lint
warning; binding its loop variables fixed it. The final checks above pass.
Initial wheel packaging emitted setuptools namespace-data warnings; explicit
`include-package-data = false` plus enumerated package data removed the warnings
while retaining installed docs and schemas.

## Limits of this evidence

At initial component delivery no root build/client integration gates were run:
another agent owns those files and checks. During PR preparation, `make quality`
was attempted and exited 2 at `format-c-check`: the unchanged baseline file
`tests/fixtures/checkpoint_ownership.c:130` violates clang-format 23.1.0 formatting.
No out-of-scope correction was made; later root quality stages did not run.
Component checks were rerun successfully on Python 3.14.7 and 3.9.6, with Ruff
0.16.6 and 0.14.0, before committing. Root native test/leak gates were not run for
this standalone standard-library Python-only change.

Linux, hostile concurrent filesystem replacement, network filesystem
semantics, actual power loss and real job launch/provider behavior were not tested.
The filesystem trust and lost-ack limitations are explicit in `README.md` and
`api.md`. TUI interaction tests use TTY-marked in-memory streams, not a real pty.
