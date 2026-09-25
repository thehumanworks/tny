# Execution server acceptance contract

Baseline: `4e4d270`, feature branch `feat/execution-server-code-mode`.

| ID | Requirement | Concrete evidence boundary |
| --- | --- | --- |
| EC01 | Exactly `run_code` on native Chat, Responses and ACP MCP; direct raw calls cannot execute | `test_execution_code_mode.py`, `test_acp_client.py`, `test_native_search.py`, `test_openai.c`: request capture and rejected direct/hosted calls |
| EC02 | Each code cell starts a distinct fresh production execution process | `test_execution_code_mode.py`: observed child `--exec-server` PID and real file/shell effects |
| EC03 | Code has no ambient file, process or module APIs | `test_code_runtime.c`, `test_execution_code_mode.py`: restricted Lua libraries; effects enter `tools.call` |
| EC04 | JSON codec and nested discovery preserve bounded tool semantics | `test_code_runtime.c`: nested empty arrays/objects; production catalog/describe/read/write cases |
| EC05 | Trusted context controls workspace, profile, permissions, hooks and identity | `test_execution_permissions.py`, `test_manifest_permissions.py`, `test_acp_managed.py`, `test_execution_code_mode.py`: approval correlation, prepared identity, hooks and readonly/profile ceilings |
| EC06 | Time, memory, printed output and nested-call counts are bounded | `test_code_runtime.c`, `test_execution_code_mode.py`: infinite-loop/allocation/output/call-limit cases |
| EC07 | Malformed frames, unknown/duplicate replies, stale identity and EOF fail closed | `test_execution_protocol.c`, `test_execution_transport.c`, production private-entry cases; `check_execution_protocol.py` proves only the actual pure admission predicate |
| EC08 | Crash/cancel/timeout do not replay effects or leave owned children | `test_execution_command.py`, `test_execution_code_mode.py`, `test_terminal_cancel.py`: observed server death, shell/grandchild disappearance and no later effect |
| EC09 | Concurrent invocations do not exchange authority or results | `test_execution_code_mode.py`: distinct contexts and same call IDs exercised concurrently |
| EC10 | Unsupported wasm and embedding configurations fail explicitly | `test_execution_library.py`, wasm branch of `test_execution_code_mode.py`, `test_acp_wasm_seam.py`; local native evidence is separate from unrun wasm CI |
| EC11 | Vendored runtime reproducible; normal release/test/quality/leak/formal gates include it | `third_party/lua/README.tny`, Make/Nix/CI wiring and the gate ledger in `evidence.md` |
| EC12 | Existing feature behavior is preserved or an explicit limitation is recorded | Full integration inventory and focused reconciliation in `evidence.md`, including ACP, images, MCP, SSH and embedded custom tools |

Fixtures use throwaway HOME/workspaces, loopback providers and synthetic
credentials. Live provider inference and account access are not required or
claimed. Test adapters emit explicit `run_code`; they do not add a compatibility
fallback to product dispatch. A passing abstract proof does not replace a real
subprocess test or certify parser/memory safety.

The source-linked proof quantifies over the pure protocol admission inputs; it
does not prove callback implementations, permission UI, parsers, allocation
safety or operating-system containment. Those boundaries have executable tests.
The command guardian owns an ancestry scope; deliberately daemonized descendants
that have already escaped that scope are not covered. No rollback is promised
for effects completed before cancellation or disconnect.
