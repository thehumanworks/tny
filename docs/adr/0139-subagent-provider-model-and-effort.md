# ADR 0139: Subagent provider, model and reasoning effort

Date: 2026-09-18. Status: accepted.

## Decision

Extend the native `subagent` tool's `create` and `message` actions with optional
`provider`, `model` and `effort` selectors. These are ordinary CLI selectors,
including user-configured native profiles and host providers. They do not accept
credentials, endpoints or host commands. Existing calls retain parent-provider
inheritance. `inspect` and `lifecycle` reject selectors.

An omitted or exact same provider inherits the parent's resolved configuration
and credentials through the owned private launch snapshot from ADR 0133.
Explicit model and effort independently override it. An unset inherited effort
is carried as `--effort default`, so child settings or environment cannot
resurrect an effort that the parent explicitly cleared.

A different provider resolves using the normal child CLI configuration and
authentication. Do not copy the parent's resolved key, URL, wire, subscription
token/account, model or effort to it. Ambient user settings and environment
remain available under normal CLI precedence. Private key/URL carriers from an
earlier child launch are removed. Permission and tool-profile ceilings, prompt
stdin, bounded output, secret wiping, cancellation and process ownership retain
their existing behavior.

Selection is per turn: omitted selectors on `message` inherit from the current
parent, not from saved child metadata. Callers repeat a different provider and
overrides when continuing it. Host children may resume through the ordinary
CLI's provider-matched host pointer. This removes the native-only child-resume
restriction in ADR 0087; the tool remains available only in native parents.
`lifecycle` reports an idle host child as resumable, subject to its provider's
usual configuration and session availability.

Selectors must be nonempty UTF-8 strings without embedded NULs. Validation runs
before permissions, extension events or process creation. Provider/model/effort
availability and accepted tokens remain the CLI/provider's responsibility;
child configuration failures use the existing safe `SUBAGENT_CHILD_FAILED`
diagnostic rather than relaying raw errors. Responses definitions retain
non-strict optional arguments under ADR 0136.

There is no new public ABI, dependency or OS seam. On wasm, create/message still
return the process seam's unsupported-context error; inspect/lifecycle remain
read-only. SSH, embedded runtimes and restricted tool profiles retain their
existing exclusions.

## Verification

Unit tests cover selector validation, including malformed UTF-8 and embedded
NULs, and host lifecycle state. The ownership suite covers independent selector
lifetimes, inheritance versus provider switching, credential carriers, wiping,
allocation faults and failure-atomic replacement. Compiled mutations check
provider routing and ignored model/effort overrides.

Local integration fixtures launch real child executables, inspect requests on
chat and Responses wires, check explicit default effort against ambient and
settings defaults, and continue the same session with different selectors. An
ACP fixture verifies selected models and host-session resume. Diagnostics tests
exercise rejected selectors and unknown providers without echoing supplied
values. Live provider credentials are not required by these tests.

The ACP no-credential fixture clears ambient subscription logins as well as API
keys. The C++ build fixture uses a unique vendor header name so a system yyjson
installation cannot replace its sentinel. These keep verification independent
of the developer's installed providers and libraries.

The buffer append helper checks the allocation pointer as well as its failure
flag before copying. This makes the existing reserve contract explicit to GCC's
analyzer and keeps an unavailable buffer on the same fail-closed path. The buffer
regression verifies that failed growth cannot expose a partial result.

### Local evidence (2026-09-18 to 2026-09-19, Linux x86-64)

The verification base is `fff9791`; the source/test diff SHA-256 is
`9497e078381b27b32a000bb7b328cca5c18816cc55dce712c5a170ff62ba6c86`.

- `make test` ran in a fresh detached worktree with the identical source/test
  diff, Nix GCC 14.4 and zsh. OpenSSL's runtime library directory was supplied
  through `LD_LIBRARY_PATH`, as in `nix/tests.nix`. All 587 unit tests and 73 of
  74 integration fixtures passed in that invocation. Its only failure was the
  TUI version assertion: a concurrent remote fetch introduced tag `v0.13.3`
  after the binary had embedded the older `git describe` value. Rebuilding the
  release and rerunning the complete TUI fixture passed without source changes.
  All 74 integration fixtures therefore have passing results; the initial
  aggregate invocation exited nonzero for that metadata mismatch.
- `make quality` passed with the pinned mise tools, Nix GCC 14.4 and zsh,
  including all 135 C and 13 C++ analyzer inputs. Clang strict warnings also
  passed. GCC 14 matches the CI compiler major; the host GCC 16 reports unrelated
  existing C++ analyzer and test-macro diagnostics.
- `make leaks` passed with Valgrind 3.27.1: 587 unit tests, CLI smoke tests,
  zero errors and no leaks. Host loader debug symbols came from debuginfod.
- `make test-subagent-mutation` passed: ownership/fault/wipe baseline and all
  14 compiled mutants killed, including the three selector-routing mutants.
- Stripped release size: 1,032,976 bytes with host GCC 16; 1,020,848 bytes with
  GCC 14. Both were below the then-current 6,000,000-byte ceiling (historical;
  [ADR 0150](0150-agent-first-harness-and-measured-footprint.md)). The separately
  supplied C++ runtime dependencies are `libstdc++.so.6` and `libgcc_s.so.1`;
  they are not included in those artifact sizes.

The change was rebased onto `2398f5e` before landing to include the concurrent
SDK workflow update. The harness source and integration/ownership fixtures are
byte-identical to the tested change. `make test-sdks` passed against a fresh
GCC 14 library: 103 Python cases (two expected packaging skips), 57 JavaScript
tests and both protocol conformance adapters. `make test-shell-workflows`
passed under Bash and Zsh. The SDK checks used an isolated cffi environment.
The complete `make quality` gate also passed again after the rebase.

Provider verification uses local protocol fixtures and real child processes.
Live provider credentials, macOS and wasm were not exercised in this local run.
