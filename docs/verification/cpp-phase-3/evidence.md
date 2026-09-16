# Evidence: C++ phase 3

Contract: [contract.md](contract.md)
State: implemented on `feat/cpp-ownership-137-139`; local native gates recorded
below on the integrated (phase 1 + 2 + 3) source. Hosted platform results
(Linux x86-64/aarch64, musl, Windows/MSYS `test_jobs_msys.py` native runtime,
wasm negative paths, Nix) are supplied by the pull request, not claimed here.
Baseline: 1d8ad71d66c06c726b3c5b35e367fec678031e85 (pre-series). Initial contract
bytes retained in contract.initial.md.
Native goal: unavailable by authorization (ordinary task, no explicit goal request).

## Implementation record — 2026-09-16

- Ownership inventory and oracle mapping: [ownership.md](ownership.md). Decision:
  [ADR 0118](../../adr/0118-runner-and-job-resource-ownership.md).
- `src/core/runner.cpp` and `src/core/jobs.cpp` replace the C sources with the
  same C-linkage entry points. `src/util/resources.hpp` provides move-only
  close-only `descriptor`/`lock_descriptor`, `pipe_pair`, `spawn_writer` and
  `process_scope` owners; heap objects use `tny::make_owned` from
  `src/util/ownership.hpp` (the local-main `src/cpp/owners.hpp` duplicate was
  not imported). OS spawn, mapped-fd staging, Job handles and pre-exec paths stay
  in `src/util/process.c`, `src/util/process_scope.c` and `src/util/jobs_host.c`.
- Retained follow-up fixes: the supervisor writes `cleanup_hold:true` ahead of
  acquiring children and latches it when any item's cleanup becomes unknown;
  the hold and the checkpoint resumable flag are updated in place without
  allocating (`jm_set_bool`, `rn_disk_packet`).
- Fixtures: `tests/fixtures/runner_ownership.cpp` binds the real runner/jobs
  sources with real descriptor, pipe and advisory-lock boundaries, an
  instrumented `alloc.c` and `tests/fixtures/resource_host_faults.c`
  (open/write/fsync/rename/dup/spawn faults around the unchanged C seams).
  `tests/fixtures/job_artifact_checks.cpp` compiles the real `jobs.cpp`.
  Unit additions in `tests/test_runner.c` and `tests/test_session_bg.c`;
  integration additions in `test_background.py`, `test_background_agents.py`,
  `test_interrupt.py`, `test_jobs.py`, `test_jobs_cleanup_hold.py`,
  `test_job_artifacts.py` and `test_jobs_msys.py` (source checks run on every
  host; native MSYS execution remains hosted).
- Build: `make test-runner-ownership`, `make test-runner-mutation`; CI native,
  musl and Windows unit lanes and Nix include the ownership fixture; Linux fuzz
  lane runs the runner mutants.

## Execution records (macOS arm64, Apple clang, integrated working tree)

Logs live under `/private/tmp/tny-finish-137-139-20260916/`.

| Check | Result | Log |
| --- | --- | --- |
| `make test-runner-ownership` | passed: 200 descriptor transfer/reuse cycles, 100 failed job acquisitions, acquisition faults (pipe/save/listener/fork), client allocation failure with allocation-free teardown, 40 item spawn/pump/reap/drain cycles, host faults preserve bytes and borrowed fds, cleanup-hold latching/refusal/release, checkpoint flag faults; descriptor count 6 before and after every loop | runner-ownership-p3.log |
| `make test-unit` (ASan/UBSan) | 567 tests, 32,019 assertions passed | test-unit-p3.log |
| `make test-runner-mutation` | see mutation results | runner-mutation-p3.log |
| `test_jobs.py`, `test_jobs_cleanup_hold.py`, `test_job_artifacts.py`, `test_jobs_msys.py`, `test_background.py`, `test_background_agents.py`, `test_interrupt.py` | see series evidence | integ-*-p3.log |

## Mutation results

`tests/mutation/runner_critical.py` (private copies, production hashes verified):
writer-before-save, close-transferred-descriptor, signal-metadata-pid,
unknown-is-complete, replay-consumed-batch and the cleanup-hold/flag mutants
carried from the follow-up fixes. Results are recorded in the series evidence
table once the run completes.

## Invariant mapping

- P3-I1: descriptor/lock owners, transfer/reuse loops, acquisition faults, host
  syscall faults; close-transferred-descriptor mutant.
- P3-I2: writer probes inside final save and socket unlink; held-lock refusal and
  byte-identical snapshot fixtures; writer-before-save mutant.
- P3-I3: restart/checkpoint fixtures in `test_background*.py`, consumed
  checkpoint stays consumed; replay-consumed-batch mutant.
- P3-I4: metadata PID is not authority (live sentinel), cancellation during
  admission/execution/finalization in `test_interrupt.py` and jobs suites;
  signal-metadata-pid mutant.
- P3-I5: `test_jobs_msys.py` source checks locally; native MSYS execution,
  musl and wasm negative paths are hosted gates.
- P3-I6: deadlines in the interrupt suite, allocation-free hold/flag updates,
  startup/size comparison in the series evidence.
