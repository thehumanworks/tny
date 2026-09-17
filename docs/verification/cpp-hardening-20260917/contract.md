# C++ ownership hardening continuation

User: continue the ownership migration for maintainability, extensibility,
reliability and high performance; artifact size must remain strictly below
6,000,000 decimal bytes. Existing ABI, failure, process and latency contracts
remain active. Do not mechanically rename the repository or optimize for old
platform-specific byte ceilings.

Discovery: the requested checkout was clean at 6b9a175. PR140 was independently
confirmed merged on 2026-09-16; current remote main is fdd5aa7. Continue in the
same checkout on new branch feat/cpp-ownership-hardening-20260917 based on that
remote commit. Preserve prior feature history, unrelated processes/worktrees,
published ADRs and all initial contract snapshots. No main merge is authorized.

## Invariants and acceptance

H1. Durable-job transactions directly own their directory string and yyjson
mutable document through standard unique_ptr aliases, not manual paired frees.
Lock acquisition/release, cancel, commit and cleanup-hold semantics are unchanged.
Destruction/early exit/reset releases document/path before the state lock;
construction failures and repeated reset are safe. Compile-time ownership traits,
real-descriptor contention tests, ownership fault suite and runner mutations prove it.

H2. Every C++ ownership module retains working path-sensitive static analysis.
GCC's C-only analyzer continues for C; existing Clang C++ analysis is an explicit
shared build prerequisite. Correct owners must pass, deliberate use-after-free
must fail, and every private C++ file must be discovered. Keep strict warnings,
ASan/UBSan, leak, fault and mutation coverage; no new diagnostic suppressions.

H3. Nix includes the files its maintained tests read. Build-only archives do
not emit missing-test-file diagnostics just while evaluating Makefile variables.
A reduced archive regression proves size/build queries still work; missing test
files must not be interpreted as a successful full test run.

H4. Public include/ and ABI bytes stay unchanged. Native release remains below
6,000,000 bytes; size accounting and boundary tests are preserved. Startup,
parser/event throughput and memory meet the existing comparisons. Tests use
fixtures, never live provider credentials or paid requests.

H5. Native full tests, quality, ABI/SDK, fault/sanitizer, ownership/fuzz/leak and
critical mutations are run with source-bound evidence. Fresh independent Fable
medium-effort review; each finding dispositioned. Push feature commits and create
a follow-up PR with truthful current local and hosted CI/Nix/SDK evidence. A
pending/unavailable/failing check is not a pass. Initial snapshot stays immutable.

## Isolation amendment

A concurrent finalization task changed the shared primary checkout branch and
edited overlapping files. Further writes are isolated in the task-owned
transaction-tree worktree/feat/cpp-job-transaction-owners. That task owns the
primary analyzer/Nix/runtime fixes and publication; this slice owns typed job
record/transaction resources, direct lifecycle regression tests and associated
mutation anchors. Initial contract text remains unchanged. No root branch,
source or unrelated process is modified after detecting concurrent ownership.
H1/H4/H5 apply directly; H2/H3 are dependency integration requirements, not a
claim that this slice alone fixes the simultaneous finalization work.

## Published integration reconciliation

The primary finalization task published cb0f74c in PR141. Its source, build,
tests and dependency inputs are now merged into this task-owned verification
branch without changing the user's primary checkout. They match PR141 exactly;
only this supplementary evidence directory differs.

H2's initially proposed analyzer-routing mechanism is superseded by the measured
combined implementation in ADR0124: retain the existing Clang and GCC checks,
make the three small ownership factories visible to GCC's analyzer by inlining,
and require positive RAII plus deliberately failing lifetime controls. This
changes the planned mechanism, not the requirement for actual C++ static
analysis, strict diagnostics or negative-control proof. The published Nix/input
fixes satisfy H3 and are covered by PR141's hosted checks.

The independent transaction review approved the ownership representation. Its
findings were actioned with a real leak-detector negative control, directory
allocation fault coverage, and explicit unwinding wording. The combined review
additionally requires fail-fast ENOMEM before JSON work on directory-copy OOM.
The final source-bound tests and measurements must use this integrated state;
pre-integration successes remain recorded as historical or component evidence,
not mislabeled as a full final-source pass.
