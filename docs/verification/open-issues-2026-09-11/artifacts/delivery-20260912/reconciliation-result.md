I will write only new reconciliation artifacts. I will check the immutable records, worker evidence, platform state, and saved goal access. I will not run builds or change processes, sources, or goals.

The initial contract hash matches, and all 88 baseline ADR-directory files are unchanged. Prefixes `0030` and `0045` still collide, so I-G6 remains blocked.

The retained-evidence scan found no exact match to current credential environment values. It did flag fixture literals, archives, and oversized or binary files for staging review. I will keep their values out of the report.

The saved goal is readable through the native API. It is still active and contains all 49 invariant IDs. I sent no goal or thread mutation requests.

QEMU PID `65737` belongs to the task cache. QMP reports it running, and a read-only SSH query reached Windows. The task-specific Docker context is also reachable, despite the default context failing. These are usable environments, not proof that the current sources pass their gates.

**Reconciliation report is ready. The overall task remains incomplete.**

Report: [reconciliation/README.md](docs/verification/open-issues-2026-09-11/artifacts/delivery-20260912/reconciliation/README.md)

| Work completed | Checks and results | Blockers |
|---|---|---|
| Immutable records | Initial contract hash matches. **88/88 baseline ADR files unchanged.** | Duplicate prefixes `0030` and `0045` remain unwaived. |
| Native goal | Read through the actual API. Saved goal remains **active**, with **49 invariant IDs** and no token budget. | No goal mutation or completion attempted. |
| Worktrees and workers | Inventoried 18 worktrees. All 10 pre-existing trees remain clean and unchanged. Four late worker manifests match **30/30, 27/27, 31/31 and 25/25** recorded files. | Identified **21 overlapping paths**. Canonical `tests/test_core.c` changed during inspection; evidence is not a final freeze. |
| Retained results | Indexed **685 run records** and 158 selected raw logs. Rechecked **627 log hashes: zero mismatches**. | F010 was not source-stable. Jobs has a help-inventory failure and lineage skip. Export mutation **X125.7a survived**. Preview failures and final platform gates remain open. |
| Platform access | Task Docker context works. Private ImageMagick 7 works. **QEMU PID 65737** is task-owned, running, and reachable over SSH; Windows version query succeeded. | Windows ARM64/x64 emulation is not native x86_64 CI proof. No builds or product tests were run. |
| Evidence safety | Inventoried 2,984 retained files without echoing secret values. Exact staging lists supplied: **2,946 text candidates, 13 held paths, 25 binary/scratch exclusions**. | Heuristic scanning cannot prove exhaustive secret absence or erase the worker-reported prior environment-log exposure. |

**Audit QA:** all 10 checks passed. Nothing was staged, committed, pushed, killed, or delegated. Only new reconciliation files were written.

Exact staging guidance: [stage-policy.md](docs/verification/open-issues-2026-09-11/artifacts/delivery-20260912/reconciliation/stage-policy.md).

**Required decision:** recommend grandfathering only the two baseline ADR collisions while preserving their bytes and requiring unique new prefixes. The tradeoff is retaining two legacy collisions. Until explicitly authorized, I-G6 remains blocked.
