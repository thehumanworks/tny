I will review the requirements and current source only. I will not run tests, change files, or read prior review material.

The source has integration blockers in ownership checks, retry failure handling, and attempt history. I am also checking credential inheritance and output reservations against the raw test coverage.

## Reject for integration

The current source has blocking ownership, concurrency, credential-isolation, and lifecycle defects. These are **static code findings**, not runtime reproductions.

No files were changed. No tests, child agents, goals, or commits were created or run.

### Findings

#### 1. P1 — The worker checks that someone holds the lock, not that its supplied descriptor owns it

**Paths:** `src/core/jobs.c:3245–3254`, `src/util/jobs_host.c:151–168`

Worker admission combines:
- matching the descriptor’s file identity;
- observing a conflicting lock through a separately opened descriptor;
- setting close-on-exec.

An independently opened, **unlocked** descriptor to the same file passes these checks while the real supervisor holds the lock. The admitted worker can then mutate the record, including through the invalid-payload failure path at `src/core/jobs.c:3276–3279`.

**Required change:** Verify/acquire the exclusive lock nonblockingly on the supplied descriptor itself before accepting ownership or mutating anything. Retain file-identity validation.

**Coverage gap:** `tests/integration/test_jobs.py:1702–1739` invokes the hidden worker without the required descriptor. It does not distinguish the actual owning open-file description from a different descriptor for the same file.

#### 2. P1 — Failed submission/retry releases ownership before writing its terminal outcome

**Paths:** `src/core/jobs.c:2434–2442`, also `1730–1737`; terminal writer at `1620–1638`

On a pre-launch failure, the caller closes `owner_fd` before releasing reservations and recording failure. During this window, another reader can project the queued attempt as interrupted. A competing retry can then acquire ownership and start a newer attempt.

`submit_finish_failed()` subsequently loads the current record and writes failure without checking the expected attempt, revision, or ownership. The old caller can overwrite the newer attempt.

**Required change:** Keep ownership through failure finalization and reservation cleanup. Make terminal updates conditional on the exact accepted attempt and expected state.

**Coverage gap:** The simultaneous-retry and revision tests at `tests/integration/test_jobs.py:1053–1170` cover preparation and the successful launch path, not this post-acceptance failure window.

#### 3. P1 — `jobs rm` can delete a concurrently retried job and its live lock files

**Path:** `src/core/jobs.c:2129–2157`

Removal checks an unlocked terminal snapshot, then releases reservations and unlinks the directory contents. It never acquires ownership or re-reads terminal state under the state lock.

A retry can begin after the terminal check. Removal then deletes its metadata, logs, and lock-file names while the supervisor holds the old lock inode. The supervisor’s transaction loop can repeatedly fail to load the deleted record without reaching its normal completion path.

**Required change:** Serialize removal with retry through the owner lock. Re-read terminal state under the state lock before deletion. Use a deletion/tombstone protocol that cannot leave a running worker attached to unlinked lock files.

#### 4. P1 — Credential isolation covers only three fixed chat environment names

**Paths:** `src/core/jobs.c:2539–2567`, `2637–2645`

An image child loses `OPENAI_API_KEY`, `OPENAI_BASE_URL`, and `OPENAI_WIRE_API`, but inherits other conversation credential carriers unchanged.

The repository supports:
- custom provider `api_key_env` names: `src/core/config.c:395–398`;
- derived provider credentials and URLs: `src/core/config.c:517–532`;
- arbitrary `--api-key-env`: `src/cli/args.c:360–364`;
- `CURSOR_API_KEY`: `src/core/config.c:956–958`.

These carriers survive the denylist. Separating the payload’s `chat` and `image` objects does not establish credential ownership in the inherited environment.

**Required change:** Build a kind-owned environment using resolved credential-carrier information, including custom and explicitly selected environment names. Remove foreign carriers before appending the child’s permitted credentials.

**Coverage gap:** The image isolation test at `tests/integration/test_jobs.py:1288–1300` checks outgoing authorization headers, not the image child’s inherited environment. The unit assertions cover the fixed names only.

#### 5. P1 — Output reservations do not close the no-overwrite race

**Paths:** `src/core/jobs.c:692–699`, `1700–1709`; `src/core/image_service.c:176–178`

Output absence is checked before acquiring the reservation. It is not checked again after obtaining ownership.

A second submitter can validate an absent destination, pause, and then acquire the reservation after the first job finishes and releases it. The second job proceeds without an overwrite grant. The image service commits with `rename()`, replacing the first successful output.

**Required change:** Revalidate destination identity and overwrite authorization after claiming the reservation and before spending. Preserve no-clobber semantics at final commit as well.

**Coverage gap:** `tests/integration/test_jobs.py:1419–1461` submits the contender while the first owner remains active. It does not cover validation before the first completion followed by reservation acquisition afterward.

#### 6. P1 — Submitter loss during acknowledgment can kill the supervisor with SIGPIPE

**Paths:** `src/core/jobs.c:3286–3300`, `src/util/process.c:224–233`, `src/util/jobs_host.c:293–297`

Process spawning restores SIGPIPE’s default disposition. The worker writes its acknowledgment before calling `tny_jobs_host_detach_session()`, which is where it ignores SIGPIPE.

If the submitter exits after transferring the payload but before the acknowledgment write, that write can terminate the supervisor. Its lifetime therefore still depends on the submitting process during this handshake window.

**Required change:** Establish SIGPIPE handling before any acknowledgment write. Treat acknowledgment delivery failure separately from ownership of an accepted job.

**Coverage gap:** `tests/integration/test_jobs.py:628–647` waits for `communicate()` to finish before attempting to kill the submitter’s process group. It does not interrupt a live submitter during the handshake.

#### 7. P2 — Attempt history points to logs that retry truncates; snapshot failures are ignored

**Paths:** `src/core/jobs.c:142–145`, `2356–2365`, `2758–2759`, `3023–3024`

Every attempt uses `item-<index>.log`. Retrying an item opens that same path with `O_TRUNC`, so an earlier attempt snapshot points to replacement diagnostics rather than its original log.

The snapshot write also ignores its return value. The new attempt can be committed even when the previous attempt was not preserved.

**Required change:** Use attempt-scoped immutable log paths. Require successful, write-once snapshot persistence before advancing the projection. Refuse rather than silently replace inconsistent existing history.

**Coverage gap:** `tests/integration/test_jobs.py:933–936` checks only snapshot existence and its attempt number. It does not check historical log bytes or snapshot-write failure.

#### 8. P1 — Cancellation reports complete cleanup without verifying the owned tree

**Paths:** `src/core/jobs.c:3126–3127`, `3158–3169`, `3178–3188`

The supervisor ignores signal/tree-cleanup return values. It observes the direct child with `waitpid()`, but does not establish that the owned descendants are gone before unconditionally recording `cleanup:"complete"`.

After the direct child exits, a descendant-held output pipe gets only a one-second drain deadline. Closing that pipe is not proof that the descendant exited. Reap errors are also converted into a reaped slot rather than an explicit uncertain-cleanup outcome.

**Required change:** Track cleanup failures and descendant ownership through cancellation. Report unknown/incomplete cleanup unless termination was verified. Do not equate direct-child exit or a drain timeout with complete tree cleanup.

**Coverage gap:** `tests/integration/test_jobs.py:770–787` checks the direct child, supervisor, and unrelated sentinel. It does not verify an owned descendant that outlives the direct child.

### Other inspected areas

| Area | Static assessment |
|---|---|
| Normal retry preparation | Owner acquisition, record reread, and revision/attempt comparison are present at `jobs.c:2236–2263` and `2342–2354`. Finding 2 concerns the later failure path. |
| Selective retry | Successful selections are refused. Carried ask session/result/log and image artifact/manifest checks are present. |
| Bounded batches | Request bounds and the active-slot concurrency guard are present. The raw integration test has an independent peak-active counter. |
| Canonical events | Ask children request `--events=jsonl`; their stdout is copied rather than wrapped in job lifecycle JSON. |
| Permission identities | Typed tools use operation-specific identities through the shared jobs service. |

**Checks:** Source and test inspection only. No test results or platform verification are claimed.

## Inspected-input hashes

SHA-256 values below are for whole files, including files inspected only in relevant excerpts. Working-tree content, not merely `HEAD`, is the review target.

`HEAD`: `b80c04b9df740c8388da03991cf4808c07e9cb50`

External requirements directory:

`/Users/tomas/projects/tny-open-issues-2026-09-11/docs/verification/open-issues-2026-09-11/`

```text
4d29e2d6fb187e48516d6b2bb707c16d3d7d3ad6d86850db93c28fa97e3664d3  contract.md
2d4a361207b7c35f769dc69315c545aa19614602b1ae511b7ce48a63258a528e  artifacts/issues.snapshot.json
```

<details>
<summary>Workspace input hashes</summary>

```text
5cda97093b0016abb77764f63681246cbb1026d0d90545f04beed95e973bfd7b  AGENTS.md
c337c59e0b5a752f424178cc816b240a073f2a611c1815333e8043b615b4cd46  docs/README.md
70a928015d0faf070cfb1c43ff772748fd25879a4942129444323141e87692c4  docs/product.md
6a76fe8f18edd722e1ae499a823624a5dcbe0038be8b9f952e354b76a2c10b71  docs/architecture.md
62c4639086c69d84e17baf1cc776f72c0a9f7adc2b9d248bd270950bd7632e5a  docs/implementation-plan.md
03b57bb8b7ab57d00e76bf4dbf8f3c1ae26aca525f556e2127ef63978626ecbd  docs/jobs.md
3b07fe2410c376f93c943011ae098c55a8662cfaeb00849a1c8ed07f271faa9d  docs/adr/0093-durable-native-jobs-and-verified-retry.md
58232aecf2099198e131a9d93524a2b2579dbb9eb0a849ea248dd7d8ac985465  src/core/jobs.c
7d3fdc91c9f9d8713705d5b356204678e7d145193e7aa2893c2184eb6b931330  src/core/jobs.h
f9cdc34861c5b5d9ca9925e43b9ec77626c0587c1451714322aef069af39a5c6  src/util/jobs_host.c
86e843d4399d85325aab857e7f0033c13279bf2f28cec746ff3473809a428e07  src/util/jobs_host.h
2adfa8a3dffebfee9539369b1fada6e6966f23b6224986fcf2a0728cd688af2a  src/util/process.c
5521fa2f4602392168609007bb2561be51e10abeb01de9591ed583024126eb66  src/util/process.h
0b1daf99fe0d6a9546649aac357b3ca8db72f4e49d32004e3eebae3725a1e88c  src/cli/cmd_jobs.c
0075d6a3f82e201ff06f7a1d21cfe9f753c54075261ace62dfeee27c4220b682  src/core/tools_jobs.c
1a780839c8388b201d37e16277a8c613217b688c961e1832dc7b21ebb556179b  src/core/config.c
9a933f52c2bf9193e2d04e5435fac0aaf83ff825de7170cefd23bb304f7534d5  src/cli/args.c
f452df7dce8a9199851a4f44c03560392fa6880afe01c25a7942be874fd7ec41  src/main.c
4362cfb1488a8c3750f4b126ae1195e0d392c5d66714383dadcede43bc0b9c6c  src/cli/cmd_ask.c
3371288a88ee1f9e8b5bbada583be6f16aa85d2bd821c0f170009042a3980ca4  src/cli/cmd_image.c
2d5285ba2e19b404c6e3099f749c2a0aa7958141e3d3f9b2de9ab90ce8930aab  src/core/tools.c
d30301ff82ddc9a635be58a5a2d716f7d956b6ff27a1a1dc4a3090b920667829  src/core/tools_ext.c
647c151485dc72bd4ffc0bff2d8f316aeef65e3800a370543c351e71bb92660b  src/core/intercept.c
1dc39179efab4205ac56374f675d1f5af51e28f9e31e03603db9c57fc5c842de  src/core/tools_shell.c
4bce048e898142f6ef83595c1d68f4d94291d27ebc445b4361f3f206f8edff11  src/core/image_service.c
07c9bfcdf5cef1845350d5cafb4b2e810996e5b8872bbc3dca7d8740ef716141  src/core/image_codex.c
bef45310cee356a33b5b1dd917370bf4360f87341051edff9c63aa769eb37711  src/core/event_jsonl.c
c870b4274e5bd47b30b70b85f030157f9d662e3ddb40dd29dbefeb44a81b80ea  src/net/net_wasm.c
13dd022bbad239cce2b87fbfa82afc317039874ccaf6fc2564967dfc59dd478f  tests/integration/test_jobs.py
833d7042e8488df498377e85e3d3a1be444e7e1bd2566f0926adbd3bfb1f0610  tests/test_core.c
97c1cef5c255d68f4fce665bd4a798cf5bc1c006af908ff44be124557d732c9f  tests/test_runtime.c
9fa90b5f3c97d75e2f28e335f82f5a9ff1b06a88edab81db63f27422341c2fcb  src/net/stream.c
eb0adceb09828628313b769f84cadc3bddf9c406ecf2320ee4c45f6bd33f5726  src/net/tcp.c
72ea6ec8975d4e8bab1148b566edef1add1c9b5f27a2d8089bde42fa20388029  src/net/http_server.c
f6f46817c7ca8021718a818e9632606c3320357c556a35728ebe9fefc615af53  src/cli/cmd_sessions.c
541e657797b8e7d08137b5ccfa4dfe22fdc4a599e0501de6778a520249b8e8e5  src/core/runner.c
e8edc4c19e1eb43ed1d7022b70a48abbd5c507746a48d3addc17ef2793545d15  src/core/extensions.c
```

</details>
