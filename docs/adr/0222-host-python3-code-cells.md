# 0222 — Code cells and tool extensions run on the user's python3

Date: 2026-11-13
Status: accepted
Supersedes: the runtime choice of [ADR 0179](0179-python-code-mode-cpython.md)
(pinned, statically embedded CPython 3.14.7) and the parts of
[ADR 0180](0180-host-authorized-python-code-cells.md) that depended on it (the
interpreter heap meter, the empty `sys.executable`, the bundled standard
library). Amends [ADR 0218](0218-python-tool-extensions.md): the tool host runs
on the same python3. ADR 0179's cell protocol, tool-call budgets and no-replay
rules, and ADR 0180's host authority, stand.

## Context

The embedded CPython was 94% of the shipped executable. A stripped Linux x86-64
release binary of `main` (eebc162d) is 27,405,120 bytes; without CPython it is
about 1.45 MB. Building it also meant pinning and compiling OpenSSL, zlib, bzip2,
xz, zstd, SQLite and libffi, and freezing the standard library.

The owner's direction: agents should have as much unrestricted capability as
possible, sandboxing is the user's choice of where to run tny, and the harness
should be as close to the user who owns the machine as possible. The system
python3 is almost always present, and it is the interpreter that user would run.

## Decision

- A code cell is `python3 -u -X utf8 -c LOADER`. `LOADER` reads one
  length-prefixed frame from fd 3 (the cell bootstrap,
  `python/tny/_cell.py`, embedded in the binary) and executes it. The bootstrap
  speaks the unchanged ADR 0179 frame protocol: the same S/C/R/F/D frames,
  limits, call budget, terminal errors and output handling. The parent side
  (`run_cell`) is unchanged apart from sending the bootstrap first.
- The interpreter is the user's own: `TNY_PYTHON` (a path, a command looked up
  on `PATH`, or `off`/`0`/`none`/`false`), else the first `python3` on `PATH`.
  A virtual environment's interpreter is kept as is, never resolved to its base.
  A candidate counts only after it runs and reports 3.8 or newer within 5 s. The
  answer is cached until `TNY_PYTHON` or `PATH` changes. On macOS,
  `/usr/bin/python3` counts only when `xcode-select -p` succeeds, so tny never
  triggers the Command Line Tools installer dialog.
- The cell runs as the user would run `python3`: no `-I`, so `PYTHON*`
  settings, user site-packages and the active virtual environment apply.
  `sys.executable` is the real interpreter. There is no heap cap; the 600 s
  deadline, 64-call budget and output limits remain.
- On a native build with no usable python3, `run_code` is not offered. The native tool catalog
  is model-facing directly, on both wires and through the ACP bridge, and a
  `run_code` call is refused before hooks run. `tny doctor` reports which
  interpreter runs cells, or why cells are off.
- Tool extensions start their host the same way:
  `python3 -c HOST_LOADER serve …`. The first stdin line carries the embedded
  `tny` package; the host needs Python 3.10 or newer and says so when it is
  older. The catalog cache key includes the interpreter path. Extensions can
  import the user's packages; with pydantic installed, schema validation uses
  it, as ADR 0218 already allowed.
- wasm, libtny and MSYS2/Cygwin builds link `code_python_unsupported.c`
  (`tny_code_python_build()` is false). They, and embedded library hosts with
  custom tools or host services, keep advertising `run_code` and refuse it
  with the existing explicit error and no direct fallback. Only native builds
  without a usable python3 switch to direct tools.

## Consequences

Measured on Linux x86-64 (glibc, gcc, `make release`, `strip`, `wc -c`):

| | `main` (embedded CPython) | this change |
| --- | ---: | ---: |
| stripped release binary | 27,405,120 B | 1,466,096 B |
| runtime libraries (`ldd`) | libc, libm, libstdc++, libgcc_s | same |
| `print(1)` cell, median of 40 | 20 ms | 30 ms |
| cell importing json, os, re, subprocess, pathlib, urllib.request, median of 30 | 55 ms | 56 ms |

The cell timings run `tny_code_run` from a driver linked against each tree's
release objects, after one warm-up cell; the host interpreter is Python 3.13.18.
The trivial cell is about 10 ms slower because the host interpreter processes
`site` and compiles the bootstrap; once a cell imports ordinary modules the
difference disappears. The first cell in a process also pays one interpreter
probe (about 40 ms here). These costs are small next to a model round trip.

The build no longer fetches or compiles any third-party library, and the source
tree drops the CPython and dependency pins, their Nix fetchers, the
`cpython_runtime.sh`/`cpython_freeze_stdlib.py` scripts and the
`python_runtime` benchmark. The installed license set is tny's `LICENSE` and
`THIRD_PARTY_NOTICES.md`.

What the model can rely on changes. The interpreter version and installed
packages are now the user's, so the instructions tell the model to check
`sys.version_info` before using recent syntax and to import what is
installed, instead of promising a fixed bundled standard library. A cell that
exhausts memory now meets the host's limits, like any program the user runs.

The integration test that kills a cell's descendants at its deadline depends
on the container's init reaping orphans quickly; in a container whose init
reaps slowly it exceeds its 1 s transport slack on both the old and the new
build. CI runs it on hosts where it passes.
