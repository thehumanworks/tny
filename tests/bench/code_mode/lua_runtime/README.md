# Historical Lua runtime (benchmark only)

Tny's production code mode used this Lua 5.4.9 runtime until ADR 0179
migrated it to Python. The files here are byte-identical to the measured
PR #197 sources (`src/core/code_runtime.[ch]` and `third_party/lua/` at
`d6e6656`) so the preserved code-mode language benchmark keeps reproducing
its Lua arm. Nothing here is compiled into shipped tny artifacts.
