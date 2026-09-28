# 0180 — Host-authorized Python code cells

Date: 2026-09-28
Status: accepted
Supersedes: ADR 0179's confinement and restricted-surface decisions (OS sandbox,
empty environment, removed builtins, native JSON facade, frozen-encodings-only
packaging, 64 MiB heap, 30 s ceiling, terminal output budget). ADR 0179's runtime
choice (pinned, statically embedded CPython 3.14.7), private cell protocol,
tool-call budgets and no-replay rules remain in force.

## Decision

The user asked that code mode "let the agent act on the host rather than adding
barriers to its success". A `run_code` cell is now an ordinary CPython program run
with the **operating-system user's own authority**: it can import the standard
library, open/read/write files, use sockets (including HTTP and HTTPS) and start
and wait for subprocesses, exactly as `python3 -c` launched by that user could.
Full access is the user's explicit design choice for their harness, not an
oversight. There is no new opt-in, confirmation step or approval workflow; default
yolo use is unchanged. Host OS access controls (file modes, users, containers,
MAC policies) remain whatever the host already enforces.

Removed, because they existed only to confine cells:

- the Linux seccomp allowlist and macOS pure-computation sandbox
  (`src/util/code_sandbox.c`), including the unsupported-host refusal;
- `RLIMIT_NOFILE=4`, `RLIMIT_NPROC=0`, `RLIMIT_FSIZE=0`, the CPU and core limits;
- the empty child environment;
- removed builtins (`__import__`, `open`, `eval`, `exec`, `compile`, `input`, ...),
  the replacement `print` and the native `json` facade (and its Lean-checked
  `tny_code_json_kind` gate, now dead code). `json` is the real stdlib module,
  still pre-bound, and `tools` is also importable.

## Authority and what mediates it

Direct Python effects are **host-authorized and not individually mediated** by tny:
no permission rule, `ask`/`auto` prompt, pre/post-tool hook, `TNY_TOOLS` profile,
read-only workspace policy, workspace extra-dir list or command sandbox
(`bwrap`/`sandbox-exec`, [permissions](../features/permissions.md)) applies to
`open()`, `subprocess`, `socket` or any other direct call. Only `tools.call`
remains mediated: nested tools keep their existing preparation, allowlists,
permissions, hooks, workspace restrictions, human-wait accounting, cancellation
and completed-effect settlement. Choosing a restrictive mode or profile therefore
restricts nested tools only; it is not a sandbox for the cell.

Placement:

- Native HTTP and verified ACP: the cell runs on the machine running tny's
  execution server, in the selected local workspace (`ctx->cwd`, e.g. `--cwd`).
- `--ssh`: nested tools act on the remote host, but direct Python still runs on
  the **local** machine in the local workspace directory; it does not follow
  `ssh_cwd`.
- libtny, wasm and MSYS2/Cygwin: unchanged explicit refusal
  (`code_python_unsupported.c`; wasm cannot start an execution server).

## Process contract

The execution server starts a fresh `--code-cell` child per cell:

- environment: the server's own (the host's), minus tny's reserved process-scope
  fields; CPython runs in isolated configuration, so `PYTHON*` variables and user
  site-packages do not reconfigure the embedded runtime, while `os.environ` and
  subprocesses see the environment unchanged;
- working directory: the workspace, sent in the start frame; `PWD` matches;
- stdin: `/dev/null`; stdout and stderr: one pipe the **parent** drains while it
  waits for protocol frames, so print, warnings, tracebacks and inherited
  subprocess output form one ordered stream and cannot reach tny's own JSON
  events;
- fd 3 (private protocol socket) is close-on-exec, so exec'd programs never hold
  it, and a `fork()` child closes and forgets it (`pthread_atfork`), so neither
  holds EOF open or can write frames; `tools.call` in a fork child raises;
- `sys.executable` is empty (there is no Python executable to re-launch) and
  `sys.path` is `[""]`: the working directory's modules import, but the frozen
  standard library is found first and cannot be shadowed;
- Python's normal signal setup (SIGPIPE ignored, so broken sockets raise);
- at the end, as at interpreter exit: non-daemon threads are joined, `atexit`
  handlers run (so `tempfile` cleanups happen), streams flush; then the result
  frame is sent and the process `_exit`s. `SystemExit` with `None`/0 succeeds;
  other codes report `error: code: SystemExit: <code>`. Uncaught exceptions
  report the summary line first, then the output with a CPython traceback that
  includes the cell's own source lines.
- The static OpenSSL looks under `OPENSSLDIR=/etc/ssl`. When neither
  `SSL_CERT_FILE` nor `SSL_CERT_DIR` is set and `/etc/ssl/cert.pem` is missing,
  the cell sets `SSL_CERT_FILE` to the first readable common system bundle
  (Debian/Arch/NixOS, Fedora/RHEL, openSUSE paths); its subprocesses inherit it.

Cleanup scope: when the deadline passes, the cell is cancelled or its protocol
fails, the parent stops the cell and every descendant it can enumerate
(`tny_process_stop_owned_tree`, Linux and macOS). A descendant that has already
re-parented itself away (double fork/daemonization) before cleanup is not found.
After a *successful* cell, background processes it started keep running with the
user's authority; the output pipe closes when the result is final, so such a
process must redirect its output (a later write to the closed pipe raises or,
without Python's SIGPIPE handling, terminates it).

## Bounds that remain

These prevent broken workflows, not host access:

- fresh process and fresh `__main__` per cell; no automatic replay;
- nested calls: 64 per cell, 256-byte names, 256 KiB JSON-object arguments,
  8 MiB results, recursive `run_code` refused; checked by child and parent;
- source 256 KiB; deadline default 5 s and now at most **600 s**, the terminal
  tool's ceiling, because subprocess builds/tests and downloads need it;
  owner prompt waits still extend it by at most five minutes;
- captured output keeps **64 KiB**: everything when it fits, otherwise the first
  bytes, `[tny: N bytes of output omitted]` and the last 16 KiB. Excess output no
  longer ends the cell; NUL and malformed UTF-8 become U+FFFD;
- the metered interpreter heap is now **1 GiB** (was 64 MiB): stdlib codecs use
  CPython's allocators (`lzma` preset 6 alone needs about 94 MiB), and the meter
  still ends a runaway allocation loop as a terminal `memory limit exceeded`.
  Subprocesses and OpenSSL's own allocations are not metered;
- terminal limits (heap, call budget, callback failure) keep ADR 0179's sticky,
  process-terminal behavior.

## Packaging: the bundled standard library

No host Python, stdlib directory or host development library is used at build or
run time. `scripts/cpython_runtime.sh`:

1. builds pinned static libraries from hash-verified tarballs listed in
   `third_party/<name>/{URL,SHA256}`: OpenSSL 3.5.8 (LTS), zlib 1.3.2, bzip2 1.0.8,
   xz/liblzma 5.8.4, zstd 1.5.7 (multithreaded), SQLite 3.53.4 (amalgamation,
   extension loading omitted) and libffi 3.8.0;
2. configures CPython with `MODULE_BUILDTYPE=static`, explicit `*_CFLAGS/*_LIBS`
   for those libraries and `PKG_CONFIG=false`, and disables modules without a
   pinned library (curses, readline, gdbm/ndbm, Tk, `_uuid`, macOS `_scproxy`), so
   a host development package can never become a dependency; the build fails if
   a module with a pinned library is not enabled;
3. freezes the pure-Python standard library with the just-built interpreter
   (`scripts/cpython_freeze_stdlib.py`, CPython's own `<frozen NAME>` code
   objects), 552 modules plus the generated `_sysconfigdata` (and, on macOS, a
   frozen `_scproxy` shim that reports no system proxy; `*_proxy` variables still
   apply). Only CPython's regression suite, Tk/IDLE/turtle, `ensurepip`/`venv`
   (which need bundled wheels and a Python executable) and `site-packages` are
   left out — none by size.

An import sweep of every frozen module in a real cell imports 534 of 549 (the
three with import-time side effects excluded); the 15 failures are curses and
dbm.gnu/ndbm (no pinned library), Windows-only modules, the terminal REPL's
readline helpers and `unittest.__main__` (runs tests). The 0179 codec limitation
(cp1252, utf-8-sig, idna, unicode_escape) is gone.

**Amends [ADR 0007](0007-linux-tls-system-openssl.md) / size-and-speed rule 4
("never static or vendored OpenSSL") for this runtime only.** tny's own provider
TLS is unchanged: still the system library, `dlopen`'d at first use, so startup and
`ldd` are unaffected. The cell's `ssl`/`hashlib` link a pinned static OpenSSL
because they must work in every lane, including fully static musl builds that
cannot `dlopen`, and without a host Python. The cost is ownership of OpenSSL
security updates: follow the 3.5 LTS series through `third_party/openssl` and
ship patched releases.

Licenses of everything embedded are installed by `scripts/install_licenses.sh`
(`make install`, both release lanes). Nix passes the tarballs through
`nix/cpython-deps.nix`; Alpine release lanes add `perl` for OpenSSL's Configure.

Measured on this Linux x86-64 host (GCC 16, release flags): the stripped `tny`
executable grows from 6,283,208 bytes (1dfee70) to the size recorded in the
[evidence](../verification/host-code-mode/evidence.md), with unchanged dynamic
dependencies (`libc`, `libm`, `libstdc++`, `libgcc_s`). Binary size is not a
ceiling (ADR 0150).

## Verification

Lean 4.30.0 checks the nine production gates in `src/core/code_policy.c`
through the Clang-AST translator (now accepting 64-bit results): the new
`tny_code_output_take` (exact specification, never exceeds the arrivals or the
head, equals "keep everything" exactly when `tny_code_output_admit` holds) and
the raised timeout ceiling, alongside the retained protocol, call, result,
output and heap gates. The UTF-8 lead classifier additionally proves its exact
byte partition, invalid-lead rejection and width bound. The gate checks 37
specifications, one helper and 22 no-wrap obligations, with 6,054 GCC/Clang UBSan
cross-check vectors and 41 rejected mutations. There are no admitted proofs,
newly declared axioms or `native_decide`; the checker reports the standard Lean
axioms used by completed proofs. Production-path tests (unit `--code-cell` children and both provider
wires) cover imports, files, environment, working directory, loopback HTTP and
HTTPS, subprocess exit/output, output bounds, tracebacks, fork, deadlines and
descendant cleanup; see the [evidence](../verification/host-code-mode/evidence.md).

## Consequences

- A model can do real host work in one cell: run builds and tests, fetch URLs,
  process archives and databases, and compose that with nested tools.
- Anything the OS user can do, a cell can do. Users who need confinement must
  provide it outside tny (a container, VM, separate account); tny's permission
  modes and profiles do not provide it for direct Python.
- `multiprocessing`'s default start method on Linux (forkserver) and
  `ProcessPoolExecutor` need a Python executable; use
  `multiprocessing.get_context("fork")` or threads. `ctypes` cannot `dlopen` in
  fully static (musl) builds.
- The executable is larger and the runtime build takes minutes longer (cached per
  build directory, ABI and flags).
