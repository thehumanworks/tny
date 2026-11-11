# 0220 — The build's shell replaces `/bin/sh` everywhere in the binary

Date: 2026-11-11
Status: accepted. Extends the store-shell decision in
[0035](0035-nix-flake-packaging.md) to code added by
[0180](0180-host-authorized-python-code-cells.md) and the TUI shell mode.

## Context

`nix run github:thehumanworks/tny` failed in `installCheckPhase`:

```text
error: Nix package retained a host /bin/sh dependency
```

ADR 0035 makes a Nix build pass the store-backed `stdenv.shell` as
`TNY_SHELL_PATH`, and the package, libtny and test derivations reject any
`/bin/sh` left in the binary. Nix is an optional developer workflow and not a
CI gate ([0137](0137-linux-macos-ci-and-optional-nix.md)), so two later
additions went unnoticed:

- `src/util/tui_shell_host.c` fell back to a literal `/bin/sh` when `$SHELL`
  is unset or relative, instead of `TNY_SHELL_PATH`.
- The embedded CPython freezes its standard library into the binary.
  `subprocess.py` names `/bin/sh` for `shell=True` (and `/system/bin/sh` for
  Android), and the generated `_sysconfigdata` module records the build's
  `SHELL`.

Both were present in the v0.38.0 glibc release binary; nothing else in it
names `/bin/sh`.

The shell check had also hidden a second break in the same phase. On Linux the
installed-package TLS check runs `tests/integration/test_https.py` from a tree
that holds only the `packageChecks` fileset of `nix/source.nix`. That fileset
lacked `code_mode_fixture.py`, which `mock_openai.py` has imported since
[ADR 0174](0174-execution-server-code-mode.md), so the mock provider exited
before it could serve a request.

## Decision

`TNY_SHELL_PATH` is the one shell every shipped path uses.

- The TUI shell mode falls back to `TNY_SHELL_PATH`.
- `scripts/cpython_freeze_stdlib.py` reads `TNY_SHELL_PATH` from its
  environment. When it differs from the default `/bin/sh`, every `/bin/sh` in
  the frozen sources becomes that path before compilation. A code cell's
  `subprocess.run(..., shell=True)` then runs the same shell as the terminal
  tool. The replacement is a plain substring replacement, so the Android-only
  `/system/bin/sh` fallback, dead on tny's targets, is rewritten too; a
  narrower match would leave its `/bin/sh` suffix in the binary.
- The Makefile passes `TNY_SHELL_PATH` to `scripts/cpython_runtime.sh` and adds
  a non-default value to the CPython archive key, so a store-shell runtime
  never reuses a `/bin/sh` archive. The default keeps existing keys.
- The path must be absolute and free of quotes, backslashes and newlines; the
  freezer rejects anything else because it splices the value into Python
  string literals.

An ordinary build (`TNY_SHELL_PATH=/bin/sh`) freezes the pinned CPython
standard library unmodified, as before. The strict Nix check stays as it is.

`packageChecks` gains `code_mode_fixture.py`.
`tests/integration/test_nix_ci_matrix.py`, which CI runs, now fails when a file
in that fileset imports a sibling module at module level that the fileset
omits.

Rejected: narrowing the Nix check to tny's own objects. A Nix-built code cell
would still run a host shell the package does not own, which is the dependency
the check exists to catch.

## Consequences

Under Nix, `subprocess` with `shell=True` runs `stdenv.shell` (bash) rather
than the host's `/bin/sh`. Scripts that rely on a non-bash `/bin/sh` must name
their interpreter. Release binaries and `make install` builds are unchanged.
The frozen standard library of a Nix build differs from the pinned CPython
sources by this one substitution.

wasm: unaffected. The wasm build has no embedded CPython and uses
`tui_shell_host_wasm.c`.

## Verification

`tests/build/test_cpython_freeze_shell.py` (run by `make
test-code-mode-language`) freezes a module with the three shapes
`subprocess.py` uses and checks that the default leaves them alone, a store
path replaces all of them, and unsafe paths are rejected. The results of the
`nix build` and override-build checks are recorded in the commit that added
this ADR.
