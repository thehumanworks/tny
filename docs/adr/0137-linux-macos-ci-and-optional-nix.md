# ADR 0137: Linux/macOS CI and optional developer Nix

Date: 2026-09-18. Status: accepted.

## Context

During the subagent regression fix (ADR 0136), the user explicitly changed
scope: remove Windows builds from GitHub Actions, remove Nix from CI, retain
Nix as an optional developer choice, and commit that policy separately.
This supersedes the Windows automation in ADR 0006 and the mandatory Nix
CI/release gates in ADRs 0035, 0085 and 0110. It also supersedes ADR 0136's
reference to the former three-workflow release gate.

## Decisions

1. Delete the Windows build jobs from both `ci.yml` and `release.yml`, their
   dependency/result checks, and future Windows release asset production.
   Retain native Linux x86_64/aarch64 glibc and static musl builds, and
   macOS arm64. Historical releases and local MSYS source/build seams are
   unchanged. Do not remove Windows-specific code or local contract tests.
2. Delete the Nix workflow. Retain `flake.nix`, its lock, `default.nix`,
   `shell.nix`, packages, dev shell, source filters and `nix flake check`
   for optional local use. Nix is neither a GitHub Actions job nor a release
   prerequisite. Using Nix tools locally for this fix does not restore a
   mandatory Nix CI dependency.
3. Automatic releases now require **ci and sdk**, both successful on the
   same commit. Update both the workflow-completion trigger and the API
   gate loop. Retain fail-closed status checks, serialized tagging, main-run
   non-cancellation, version calculation and explicit release dispatch.
   The release publisher still waits for Linux/macOS packaging and complete
   SDK validation before publication. No gate bypass or manual unverified
   release is authorized by this change.
4. Linux x86_64 retains the full native integration suite. Linux aarch64
   and macOS retain their existing unit, ownership, fault, ABI and packaging
   checks. Removal of Nix means they no longer have a separate hermetic
   full-suite CI run. Do not silently move the historically two-hour hosted
   macOS suite into its 30-minute build job. Developers may run local Nix
   checks; full per-platform CI can be reconsidered separately.
5. Preserve real ImageMagick 7 conversion coverage, formerly supplied by
   Nix, on the macOS native lane. Require the `magick` major version, install
   through Homebrew when missing, and run the existing image-export suite.
   Ubuntu's converter-independent cases remain; no product dependency is
   added. The Linux-hosted wasm/browser parity jobs remain, since the user
   removed Windows/Nix automation, not browser support.
6. Replace the Nix-matrix lockstep test with a policy test for Linux/macOS
   runners, no Windows/Nix jobs, complete remaining release dependencies,
   retained flake systems/files and optional-developer documentation. Keep
   the existing test path/runner. Include the workflows and newly inspected
   local Nix files in `nix/source.nix` so optional sandboxed checks remain
   self-contained. Update packaging gate tests and published site claims.

## Verification and tradeoffs

The replacement policy test fails against the old tree because its Nix
workflow still exists. It must pass after removal, alongside the automatic
release and SDK publication contract tests, actionlint, site regeneration
checks and Python lint. GitHub Actions on the final commit must demonstrate
that only Linux/macOS runners execute, that no Nix workflow starts, and that
the ci+sdk release gate publishes successfully.

The previous main run had an unrelated MSYS permission-test failure. A local
workaround was investigated but deliberately dropped after this steer; no
permission behavior or test weakening is included to make Windows pass.
Branch protection was checked: main had no required-status protection or
active repository ruleset to update. If one is added later, require `ci`
and `sdk`, not a retired Nix check.

This narrows automated platform assurance and future downloads; it does not
claim Windows or all local Nix configurations are tested or supported by the
remaining release matrix. The subagent fix and this CI policy are separate
commits as requested.

## Outcome

CI `35384412158` and SDK `35384412077` passed for policy commit `87f5cfb`.
Only Linux/macOS runners were scheduled. Auto-release `35389142706` tagged
that commit after the two gates succeeded, and release `35389156075`
published `v0.13.0` with no Windows asset. Local policy, release-contract,
site and filtered optional-Nix-source checks passed. The full evidence and
published archive verification are in
`docs/verification/subagent-tool-schema.md`.
