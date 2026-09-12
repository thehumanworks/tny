# 0103 — Preserve linker-created Nix executable layout and gate installed payloads

Status: Accepted — 2026-09-12.

## Context

The Linux glibc executable budget remains strictly below 1 MiB, and packaged builds
pay that same budget (`docs/size-and-speed.md`). Nix currently checks the stripped
link output before fixup. On the final aarch64-linux candidate it is 986,088 bytes,
but adding OpenSSL's RUNPATH with patchelf after stripping creates an additional
64 KiB-aligned LOAD segment and grows the installed executable above the budget
(1,052,872 bytes for the normal version-stamped derivation). Wrapped and unwrapped
packages contain the same enlarged executable payload.

OpenSSL must remain loaded lazily through dlopen. Linking libssl or libcrypto as
DT_NEEDED dependencies would change startup/runtime policy. Reducing the 64 KiB ELF
page alignment, dropping unwind tables or increasing the budget would weaken
other guarantees and is not an acceptable correction.

The pinned nixpkgs patchelf 0.15.2 setup hook uses dontPatchELF only to suppress its
automatic --shrink-rpath pass. That pass otherwise drops the intentional OpenSSL
directory because no DT_NEEDED entry names an OpenSSL library, forcing the
current postFixup re-addition and ELF relayout.

## Decision

For the Nix **CLI package on Linux only**, provide the OpenSSL library directory
through NIX_LDFLAGS as a RUNPATH at link time. Set dontPatchELF on that package to
retain the intentional dlopen-only directory, and remove its postFixup patchelf
addition. Keep the existing makeBinaryWrapper behavior. Darwin uses its existing
SecureTransport path and receives neither Linux setting. The separate libtny
package is unchanged by this decision.

Retain the ordinary linker-selected interpreter, 64 KiB LOAD alignment, unwind
sections, hardening, compiler flags and DT_NEEDED libraries. No global Nix setting
changes. dontPatchELF suppresses only the pinned shrink hook; if the hook's scope
changes on a future nixpkgs update, its effects must be reviewed again.

The package retains its existing pre-fixup make size-check. Its installCheck also
measures the actual installed ELF/Mach-O payload: `.tny-wrapped` when the compiled
wrapper exists, otherwise `bin/tny`. Obtain the applicable SIZE_MAX from the
existing Makefile by adding a command-line query target with make --eval; do not
copy platform constants into Nix. Refuse a nonnumeric/empty limit and an installed
payload whose byte count is at or above that limit. This checks both wrapped and
unwrapped packages and preserves Makefile ownership of platform budgets.

Retain installed version/help/doctor checks. On Linux, run the maintained HTTPS
fixture against the installed executable with test-only Python/OpenSSL/tini
inputs, an adopting reaper and no LD_LIBRARY_PATH workaround. Declare its two
actual source inputs (test_https.py and mock_openai.py) in the Nix source filter.
It must complete a trusted native streamed tool turn, reject the untrusted
self-signed certificate and retain the TLS EOF/backpressure cases. The runtime
package still does not link OpenSSL or depend on a test runner.

## Evidence and tradeoffs

Sandboxed disposable overrideAttrs experiments used the same frozen candidate,
locked nixpkgs and supplied version. The instrumented baseline grew from 986,088
to 1,052,896 bytes. Link-time RUNPATH variants remained 986,088 bytes after install;
each installed payload is byte-identical to its own linked payload. Their two
LOAD segments retain 0x10000 alignment, while baseline post-fixup has a third.
DT_NEEDED is unchanged and names neither libssl nor libcrypto. Both wrapped and
unwrapped candidates passed the actual installed HTTPS fixture plus
version/help/doctor. Exact derivations, flags, headers, section inventories,
binaries and logs are under the task's nix-corrections/package-proofs evidence.
These are candidate results, not final combined-tree or other-platform proof.

Disabling shrinking retains the linker's existing package-local lib directory
and GCC runtime search directory in addition to glibc and the intentional
OpenSSL directory. They are immutable Nix store paths, not host or user search
paths. This favors preserving the linker-created layout over pruning unused
search entries. No identical dependency-closure claim is made; closure and
runtime-path changes remain visible in derivation/readelf/path-info evidence.

The compiled wrapper's own small executable is not substituted for the actual
tny payload when measuring size. Passing the pre-fixup gate alone is no longer
sufficient evidence that the installed package meets the documented budget.

## Required verification

- Build wrapped and unwrapped packages with sandbox=true; both size gates,
  exact version/help/doctor and actual installed Linux TLS fixtures must pass.
- Show that the installed gate rejects a genuinely oversized payload while
  deriving the unchanged limit from Makefile; retain the old failing artifact.
- Preserve interpreter, LOAD alignment and unwind data, and show no new SSL or
  crypto DT_NEEDED entry. Preserve source/config/dependency hashes and flags.
- Evaluate the Darwin branch and run the applicable actual hosted Nix matrix;
  aarch64-linux results do not prove x86_64-linux or aarch64-darwin behavior.
- Keep A11's independent Linux test-only adopting reaper and complete the full
  final flake checks after all approved fixes are integrated.

This narrows ADR 0035's post-fixup RUNPATH mechanism for the CLI package and closes
its installed-size measurement gap. It changes no size budget, eager-loading
policy, public API or unrelated package.
