# Subagent tool schema regression — 2026-09-18

Decision: ADR 0136 (Responses optional arguments). Baseline:
`89bcd5918da0e225e1806813a206d007daafac0a`.

## Red/green reproduction

The running baseline harness returned the reported diagnostic for one direct
`subagent` call with `action=create`, `id=""` and a nonempty prompt. Its
model-visible schema required all three properties, contradicting its own
instruction to omit `id`. No child was created by that rejected probe.

The updated `tests/integration/test_subagent.py` fixture emulates a
schema-conforming model after Responses' default strict normalization. The
same parent creates two independent children, then continues and reads them.

| Binary / wire | Result |
| --- | --- |
| Unchanged baseline / Chat Completions | Two children, eight successful tool calls |
| Unchanged baseline / Responses | Both creates returned `SUBAGENT_INVALID_ARGUMENT: create allocates the child id; omit id` |
| Fixed / both wires | Two children and all eight calls successful on each wire |
| Fixed / Codex Responses profile | Existing credential/inheritance create scenario passed |

The baseline `responses_tools_flatten` and
`responses_tools_preserve_optional_and_explicit_strict` unit tests both failed
on their strict-setting assertions. Both pass after the translation change
(267 assertions). The subagent validation test passes with 83 assertions,
including continued rejection of explicit empty and null IDs on create.

Commands:

```sh
make debug release
build/tny-test -s core_suite -t responses_tools
build/tny-test -s core_suite -t subagent_prepare_rejects
python3 tests/integration/test_subagent.py
python3 tests/integration/test_subagent_diagnostics.py
python3 tests/mutation/mutate.py --focus responses-optional-tools --test responses_tools --fast
```

The focused mutation run compiled both mutations (`||` to `&&`, and default
`false` to `true`). Both were killed by unit tests: **2/2 killed, 0 survivors,
0 invalid**, followed by a successful restored baseline.

## Real multi-agent run

This was a fresh parent process from a copy of the rebuilt binary, not a mock,
not `tny ask` children manually launched by a shell, and not a claim that the
already-running old harness was hot-patched. The parent model called the real
`subagent` tool eight times. Children used the same binary and real Codex
subscription through the native Responses loop. Calls are synchronous under
the existing contract; this is multiple independent agents, not a claim of
parallel scheduling.

- Provider/model: `codex`, `gpt-5.6-sol`, effort `low`.
- Parent session: `e875f416c7e3ece4`; exit code **0**.
- Parent output: `MULTI_AGENT_OK`, followed by both child IDs.
- All eight tool records have `name=subagent`, `status=success`.
- Create arguments omit `id`; inspect/lifecycle arguments omit `prompt`.
- Child transcripts contain no tool calls or further delegation.

| Child | Create result | Message result | Stored state |
| --- | --- | --- | --- |
| `41f9b88e69874658` | `ALPHA_OK` | `ALPHA_FOLLOWUP_OK` | Two turns, `done`, exit 0, not running, resumable |
| `e92aa60e20d61c06` | `BETA_OK` | `BETA_FOLLOWUP_OK` | Two turns, `done`, exit 0, not running, resumable |

The exact sanitized arguments, tool results and observed child metadata are in
`subagent-tool-schema-live.json`. No credentials or private provider payloads
are included. Child updates were recorded at 18:37:51Z and 18:37:55Z on
2026-09-18. User settings were restored byte-for-byte in a `finally` block.
The workspace was temporary and the parent had a 16-step ceiling.

Reproduction shape (with a real Codex login and a scratch cwd):

```sh
TNY_TOOLS=all tny --cwd "$scratch" --provider codex --model gpt-5.6-sol \
  --effort low --max-steps 16 ask --json --stdin <<'PROMPT'
Use only the subagent tool. Create two children, asking them to reply exactly
ALPHA_OK and BETA_OK without using tools. Omit id on create. Message each
returned id once for ALPHA_FOLLOWUP_OK and BETA_FOLLOWUP_OK. Inspect and
lifecycle each child, omitting prompt. Stop on any tool rejection. After
exactly eight successful calls, reply MULTI_AGENT_OK and both child ids.
PROMPT
```

Baseline binary SHA-256:
`85ac3f2500746aedb51a8c0331a7b97009134b5ef1a57ac747a71ee97e69efa5`.
Live-tested binary SHA-256:
`277849b37c4b3be547be1cd410e394e3d821e2d7f3e59ab06a4aa2867e8b2b75`.
The stripped native artifact is **1,024,784 bytes**, below 6,000,000 bytes.
C++ runtime dependencies are external `libstdc++.so.6` and `libgcc_s.so.1`
(in addition to libc/libm/loader). No speed claim is made.

## Full verification and environment

The first host run exposed verification-environment problems rather than the
subagent regression: missing Zsh/Valgrind, GCC 16 analyzer/build-fixture
findings in unchanged code, a Valgrind/stripped-host-loader mismatch, and an
ACP no-credential fixture that inherited an ambient Claude OAuth token.
These attempts are not reported as passes. The GCC 16 volatile-counter
warning received the one-line test-only fix described in ADR 0136.

Repeat verification uses an isolated worktree with the same patch, an empty
HOME/config directory, provider credentials removed from the environment,
Nix GCC 13.4 for runtime checks, GCC 14 for the CI analyzer lane, Zsh 5.9.2 and Valgrind
3.27.1. Formatting/lint tools retain the repository's `mise` pins. Nix-built
binaries use a matching libc and an OpenSSL RUNPATH, not a global library-path
override in the user's environment. No warning suppression or release-gate
bypass is used.

`make leaks` passed with Valgrind: all 574 unit tests and four CLI smoke
commands passed, with zero memory errors and zero definite/indirect leaks.
`make warn-strict CC=clang CXX=clang++` passed. The Zsh quick-ask and Bash/Zsh
workflow suites passed after installing the missing Zsh dependency.

The initial Nix compiler invocation used an obsolete unsuffixed linker flag
variable. After using its target-suffixed variable, the non-sanitized leak
build found OpenSSL. ASan's intercepted `dlopen` additionally needs a
a scoped OpenSSL-only library lookup path even with a transitive RPATH;
the final isolated test environment supplies it (no libc override). These are isolated verification binaries, not
changes to tny's build or release flags.

The initial host `make test` completed with failures in credential isolation,
host-header-dependent C++ fixtures and an unchanged GCC 16 fault-fixture
warning. The isolated runtime unit suite passes all 574 tests. The isolated
full suite still needs its supported-toolchain CI result; a host-installed
`yyjson.h` also shadows the throwaway vendor header in its clang-tidy fixture.
Local `make quality` completed its format, lint, strict-warning and clang-tidy
checks, but the Nix GCC 14 analyzer reported an unchanged `buf_append` path.
Those local aggregate attempts are **not** claimed as passes. The clean
GitHub Actions CI/SDK gates are required for final release eligibility.

The user's subsequent CI policy change is a separate commit and ADR 0137.
Its policy regression, 22 automatic-release tests, eight SDK publication
contract tests, toolchain-pin check, actionlint, site regeneration and browser
JavaScript tests pass locally. Evaluating the optional Nix test source and
running the policy test from that filtered source also passes. Windows and
mandatory Nix automation are removed by explicit user request, not as a
workaround for a subagent test failure.
