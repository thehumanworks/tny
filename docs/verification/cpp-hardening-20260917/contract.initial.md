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
