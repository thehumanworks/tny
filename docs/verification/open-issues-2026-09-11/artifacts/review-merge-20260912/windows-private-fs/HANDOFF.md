# First primitive slice — stopped at unmet independent review checkpoint

Worktree: /Users/tomas/projects/tny-open-issues-private-fs-2026-09-12, branch feat/msys-private-filesystem, base f968265. Parent finalized ADR0105/A27 before product writes; inputs.json records seeded current retry test, ADR0104/0105 and contract copies. Parent's later runner edits remain in canonical source and are untouched.

## Frozen deliverable

first-slice/patch.diff SHA e1aa9bc22b1816aa8056cc7bb093d2c86630581f930c3869a705ccbcfc3399a1; manifest.json SHA05fc6bab1699aa4067b1dfe78df3d1827a0a717f55f730624b9f55be0bced816. Five exact before/after files: new src/util/private_fs.c/h, new tests/fixtures/private_fs_primitive.c, Makefile, nix/source.nix. Final live hashes were rechecked against frozen snapshots after all checks. No caller files or canonical product source were changed.

The internal primitive duplicates already-acquired parent handles, creates literal exclusive file/directory entries with creation-time restricted Windows ACLs, verifies native owner/ACL/type, admits a separately owned CLOEXEC fd only after native identity equality, and distinguishes kept entries from created-object abort cleanup. API loading is lazy system DLL resolution. POSIX behavior uses existing syscalls; no new public ABI. This is a first implementation slice, not a completed private filesystem subsystem.

## Ordinary checks completed

- Mac: make -B test-private-fs SANITIZE=1,78 checks pass with ASan/UBSan.
- Linux container: same target,78 checks pass with ASan/UBSan.
- Actual existing Windows ARM guest/x64 MSYS2: make -B test-private-fs SANITIZE=0 TNY_VERSION=f968265,132 checks pass. Independent restricted-token AccessCheck denies data read/write/append, DELETE, WRITE_DAC/WRITE_OWNER and directory DELETE_CHILD before content, with deliberately broad positive controls; current user can read/write but cannot execute a0600 file. Parent/name replacement, foreign-entry preservation, actual filename reopen/read/write, retained-object abort and keep behavior pass.
- A disposable compiled fault grants FILE_EXECUTE to the owner of a private file. The distinct mutant binary fails the independent owner-execute assertion; the unchanged original binary then passes132 checks. See mutation descriptor and native logs. No mutant was applied to the worktree.
- clang-format23.1.0 dry-run passes. Production primitive object import inspection shows no eager advapi32/NT imports; the standalone fixture links advapi32 only for its independent access oracle.

The Linux archive's initial root-owned0600 copy caused a permission failure before build. It is preserved in linux-archive-permission.log; changing ownership of that task archive alone allowed the final run. Partial-source guest/container copies produce missing-Git/test_main metadata-probe messages from unrelated Makefile parse-time inventory, but the selected primitive compile/execute target succeeds. No full application or full unit suite result is claimed.

## Stop condition and remaining gates

The automatic content filter stopped the assigned fresh source-review worker. Required first meaningful source review is UNMET. Parent instructed no retry/rephrase/alternate-agent attempt and no further primitive expansion or six-caller rollout. No such attempt was made. All already-running ordinary checks are collected; no commands remain running.

Pending after a permissible independent review: resolve findings; existing-private-role reuse policy; complete sys/native/explicit+implicit.lnk observation; mount-aware native confined traversal; native publication/committed-state ownership; opaque transform stage and proven pathname pin through converter quiescence; six caller integration; full failure/exhaustion/mismatch matrix; actual complete guest/product flows and hosted native x64 CI; Mac/Linux/WASM parity and unchanged budgets. Stage pinning is not proved by this slice.

As primary requested, the next reviewed wiring delta must add a meaningful nix/tests.nix gate/inventory note explaining existing make test→test-unit→test-private-fs inclusion, without duplicate execution. Final Nix execution must show primitive counts. Only nix/source.nix is changed in this frozen slice.

Do not apply the patch as a complete platform fix or treat the approved design as approval of this unreviewed implementation. Preserve the review blocker, original hosted Windows failures, initial diagnostic findings and all remaining obligations. No goals, children, installs, global remounts, automatic ACL repair, commits or pushes were performed.
