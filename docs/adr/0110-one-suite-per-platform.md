# ADR 0110: Each suite runs once per platform; main runs are never cancelled

Date: 2026-09-13. Status: accepted.

## Context

Between 2026-09-08 and 2026-09-13 no merge to `main` produced a release
(ADR 0085) even though the release machinery itself was sound. Three
independent causes stacked up:

- The native `darwin-arm64` lane of `ci.yml` ran the whole `make test`
  suite on a hosted `macos-15` runner and needed 2 h 10 min per run
  (`test_openai.py` alone 36 min, `test_cursor_management.py` 19 min),
  against 21 min on Linux and about 23 min for the *same* suite inside the
  sandboxed `nix-darwin-arm64` job. The release build repeated the suite
  on the tag, so `v0.9.0` took 2 h 28 min on macOS.
- Every workflow used `cancel-in-progress: true` for `main` as well as
  pull requests. Any push within those two hours cancelled the previous
  main run, and `auto-release` requires `ci`, `nix` and `sdk` to be
  *green* on the same commit — a cancelled run never turns green.
- Two merges that skipped pull-request CI landed real failures: five
  `image_service_suite` unit tests assumed POSIX symlink and mode-bit
  semantics that MSYS2 does not have (it copies the target instead of
  linking unless `winsymlinks` is set), and the new stream-interruption
  fixture's reset-socket case cannot hold on wasm, where an errored
  `fetch()` stream discards the chunks it had not yet handed out.

The suite was also running two to three times per platform per push
(`ci` build lane, `nix` check, and again in `release.yml`), and `tnytty`
occupied three more runner slots for a one-minute job.

## Decision

1. **One full suite per platform.** The `linux-x86_64` lane of `ci.yml`
   runs `make test` natively (gcc, ASan, the developer's environment). The
   `linux-aarch64` and `darwin-arm64` lanes run the unit suite, the shell
   workflow tests, the libtny fault and fuzz smokes, size, packaging and
   the ABI/libtny staging — everything that produces or checks an
   artifact — and leave their full fixture run to the hermetic `nix`
   workflow, which already covers all three systems (ADR 0035) in a
   fraction of the time. The macOS-only mock and timeout overrides that
   tried to make the hosted run finish are gone with it.
2. **`release.yml` does not test.** A tag exists only because the three
   gates were green on its commit (ADR 0085). The release build runs
   `make release`, size, smoke and packaging, then the SDK certification
   whose conformance reports are release assets. A hand-pushed tag on an
   unchecked commit gets no safety net; the docs say so.
3. **Main runs are never cancelled.** `ci`, `nix` and `sdk` cancel only
   superseded pull-request runs (`cancel-in-progress:
   ${{ github.event_name == 'pull_request' }}`). A main run always reaches
   a conclusion, so `auto-release` always gets its answer.
4. **`tnytty` rides in the build lanes** as a step of each glibc/Darwin
   build job instead of its own three-runner matrix.
5. Platform rows the platform cannot honour are guarded, not deleted: the
   symlink and mode-bit assertions in `tests/test_image_service.c` probe
   the host the way `tests/test_core.c` does, and the wasm run of
   `check_stream_interruption` records the reset-socket case as a
   transport limitation (`docs/verification/stream-interruption.md`).

## Consequences

- Wall-clock to a green `main`: the critical path drops from the 2 h+
  macOS lane to the ~25 min `nix`/`sdk` macOS jobs; a release run drops
  from ~2.5 h to the ~30–40 min SDK certification on macOS.
- Fewer macOS runner slots per push (three `tnytty` jobs gone, no
  `darwin-arm64` fixture run), so the remaining macOS jobs queue less.
- The non-sandboxed run of the fixture suite on aarch64 Linux and on
  Darwin is no longer part of `ci`; a bug that only shows outside the nix
  sandbox on those two platforms is caught by `linux-x86_64` or by a
  developer's `make test`. That trade is deliberate and recorded here.
- `docs/ci.md` describes the lanes; `test_nix_ci_matrix.py` and
  `test_toolchain_pins.py` still pin the nix matrix and the tool pins.
