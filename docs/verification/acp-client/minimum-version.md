# ACP minimum-version verification

Local macOS validation, 2026-09-27. Base revision: `4396a1f2`.
The code/test/build diff against that base has SHA-256
`3c29507f45281a25680ea8b2067398eaaa34fd94705213690a65456e7662998c`
(`git diff --binary 4396a1f2 -- src tests Makefile .github/workflows/ci.yml nix`;
documentation excluded). Contract: [ADR 0177](../../adr/0177-acp-minimum-compatible-version.md).

## Installed adapter investigation

Mise resolves `npm:@agentclientprotocol/claude-agent-acp@latest` to `0.81.2`.
Both its `--version` output and an initialize-only exchange reported that
version. Initialize also reported the required package name, ACP protocol `1`
and `loadSession: true`. Source inspection confirmed tools-only option forwarding
and rebuild-on-option-change behavior for resumed sessions. No live Claude
model turn was run. This is not a claim that every future adapter release has
been tested; see the compatibility assumption in the ADR.

The verified pre-commit stripped native Release artifact is **1,535,952 bytes** (`wc -c`), with
runtime dependencies `libc++.1.dylib` and `libSystem.B.dylib` (`otool -L`). This
is a measurement, not a size or performance comparison.

## Checks

| Check | Result |
| --- | --- |
| `PATH="$HOME/.elan/bin:$PATH" make verify-acp-proofs` | Passed. Lean 4.30.0, 34 version fixtures and 18 handshake transitions; no proof holes or additional axioms. |
| C version-policy tests | Passed, including exported Lean decisions, missing/malformed values, uint32 bounds and 2,664 numeric/build-metadata comparisons. |
| ACP client and managed integration fixtures | Passed on final-source standard Release, both in an isolated worktree and again in the primary workspace after rebuilding: 34 client and 19 managed tests. |
| `make quality` | Passed after splitting parser cursor increments out of compound conditions. GCC analyzer explicitly skipped on Darwin, as designed. |
| `make leaks` | Passed on final C source, including the new suite; documented macOS fork-suite exclusions remain. |
| `make test` | Passed (exit 0): 601 unit tests passed, one unit skip, and all 101 integration-script invocations passed (with their documented platform skips). |
| Scoped admission mutation checks | Passed on the final-source isolated worktree: 17/17 valid mutants killed (16 by unit tests, one handshake mutant by integration); four uncompilable mutations, no surviving valid mutants or timeouts. One equivalent overflow-return mutant is documented in the harness: without advancing the cursor the caller still rejects it. |

Mutation selection used the existing harness: all `acp_compat.c` targets plus
the `acp_client.c` target matching `intact && acp_claude_tools_only`, with
`--test acp_versions` and the integration fallback enabled. The isolated
worktree kept temporary mutations away from the running full-suite checks.

The full suite started before the final readability-only cursor refactor. The
affected unit/wire/mutation checks are repeated against the final source.
An auxiliary wire run against `build/leakcheck/tny` failed its extension-hook
case because that nested build location does not find the repository Python
extension host. It is not the shipped layout; final wire validation uses the
standard `build/tny` Release artifacts in both workspaces, without changing
extension behavior. Their code/test/build diff hash matches the one above.
The full suite's generated website version/size substitutions were restored;
no authored website changes are part of this fix.

Raw local logs: `/tmp/tny-acp-{test,quality-final,leaks-final}.log` and
`/tmp/tny-acp-isolated-{mutation,wire}.log`. The final primary-workspace unit,
proof and wire rerun is `/tmp/tny-acp-final-validation.log` (exit 0).
Lean verifies the specification; exported
fixtures connect it to production behavior on those inputs. These checks do
not prove C memory safety, upstream SDK behavior, process isolation or live
account inference.
