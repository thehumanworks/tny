# Linux verification record

Source revision: `e47c9cd51f0796933238ace4d53aced1efd73c4d`.
Environment: disposable CPU-only Modal sandbox, Linux x86_64, gVisor kernel,
Debian Clang 14.0.6, Python 3.14.2. No provider credentials or public services.
The sandbox was explicitly terminated after collecting the results.

| Check | Actual result |
| --- | --- |
| `make -j2 BUILD=build-clang CC=clang CXX=clang++` | Exit 0; no warning/error diagnostics; stripped binary 1,110,016 bytes |
| `test_collective_swarm.py` | Exit 0; 13 passed; 7.715 seconds |
| `test_collective_cap.py` | Exit 0; 1 passed; 1.312 seconds |
| `test_swarm_delivery.py` | Exit 0; 5 native passed, 1 wasm case skipped; 9.807 seconds |
| `test_team_mailbox.py` | Exit 1; 32 passed, 1 failed; 3.975 seconds |

The mailbox failure was the existing `test_host_parent_path_handling` assertion:
a symlink parent unexpectedly passed the directory-open restriction. An independent
Python probe, without tny loaded, reproduced the host behavior: the path was a
symlink, and `os.open` with `O_DIRECTORY` (65536) plus `O_NOFOLLOW` (131072)
unexpectedly succeeded. The descriptor was closed and temporary probe files removed.
This is a demonstrated gVisor host limitation, not a complete Linux-suite pass.
The test was not weakened or skipped. All new inotify wait, quiet-no-rescan,
cancellation, race, atomic-replacement and directory-loss tests passed.

An earlier GCC 12.2 build attempt failed on the pre-existing runner.cpp diagnostic
pragma for `-Wanalyzer-fd-leak`, unsupported by that compiler. The subsequent Clang
build retained warnings-as-errors. No product source workaround was added.

Tool result identifiers (execution audit, not public links):
Clang build `fc-01M2XWPR4VG305KM440RSDD2BS`; targeted suites
`fc-01M2XWXCR3XH12ZPNVKF3CY6JN`.

Later source change through `25993c6` only makes publication comparisons explicitly
compare against zero. Later test changes accept the integration runner's binary
argument and format an existing assertion. This record does not claim a new Linux
build of those later revisions, hosted-GitHub Linux completion, browser runtime,
provider cache billing or real-model quality gains.
