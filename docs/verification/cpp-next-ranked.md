# Next C++ ownership conversions (2026-09-17)

Baseline: main 4e760be, after merged PR140 and PR141. Ranking weighs demonstrated
unchecked ownership/failure paths, user impact, dependency leverage and bounded
verification cost; allocation/free counts are a hotspot heuristic, not defect counts.

| Rank | Issue | Boundary | Reason |
| --- | --- | --- | --- |
| 1 | [#142](https://github.com/thehumanworks/tny/issues/142) | Checkpoint reconstruction/recovery | Inconsistent copy checks and allocating caller-state rollback in a bounded restart-policy boundary. |
| 2 | [#144](https://github.com/thehumanworks/tny/issues/144) | Native provider request/pending-turn storage | Hot path, retries/cancellation and multiple retained pending states; excludes migrated parsers/events. |
| 3 | [#145](https://github.com/thehumanworks/tny/issues/145) | MCP connection/warm-up transfer | Cross-thread ownership and abandonment require precise quiescence/slot-lifetime tests. |
| 4 | [#143](https://github.com/thehumanworks/tny/issues/143) | Session/transcript/result storage | Broad lifetime reach and retained JSON; larger direct-field consumer surface. |
| 5 | [#146](https://github.com/thehumanworks/tny/issues/146) | Configuration/profile transactions | Widespread replacement/precedence and secret ownership; largest compatibility surface. |

Only #142 is selected for implementation in this task. Recommended order is not
an assertion that every later issue is technically blocked on its predecessor.
The original checkout and unrelated issues/worktrees are preserved.

The independent Fable design review confirmed #142 first and promoted request
cleanup and MCP handoff over the broader session-storage boundary. The issue
titles and this table reflect that final ranking; no scope or gate was removed.
