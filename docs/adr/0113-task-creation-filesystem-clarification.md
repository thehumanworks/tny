# ADR 0113: Task-creation filesystem clarification

- Status at creation: accepted
- Date: 2026-09-14
- Clarifies: [ADR 0112](0112-bundled-task-creation.md), filesystem paragraph
- Requirements: I2, I4 in [the contract](../verification/task-creation/contract.md)

The independent review identified an overbroad use of "browser/wasm" in ADR
0112. Only the browser build uses temporary MEMFS. The Node wasm build is
linked with `-sNODERAWFS` and reads and writes the host filesystem.

The task-creation instructions and user documentation therefore limit the
MEMFS persistence warning to browser builds. The remaining task-creation
architecture is unchanged. A general wasm warning was rejected because it
would incorrectly describe tasks created through the Node CLI. No platform
code or new filesystem behavior is introduced.

The shared integration fixture is used by Node wasm CI; native/live tests,
source inspection of the Makefile and review resolution are recorded in
[the evidence](../verification/task-creation/evidence.md). ADR 0112 remains
unchanged as a historical record; this decision supersedes its filesystem
statement only.
