# 0196 — Parallel read-tool batches

Date: 2026-10-19
Status: accepted

## Goal

Overlap independent tool reads without sharing mutable runtime state or racing
permission, extension, session and cancellation ownership.

## Decision

`tools.parallel(calls_json)` accepts a nonempty JSON array of objects with
`name` and `arguments` fields, and returns JSON text containing each text result
in input order. Each entry consumes the existing per-cell tool-call budget.
The complete shape is validated before any effects. Recursive batches and
`run_code` entries are refused. The normal `tools.call` remains synchronous.

Contiguous local `read_file`, `list_files`, `glob_files`, `grep_files`,
`file_info`, `read_tool_result` and `web_fetch` calls overlap in groups of at
most four fresh native workers. Every other tool is a serial barrier, including
terminal, subagent, MCP, custom callbacks, mutations and SSH operations. The
caller explicitly declares that the reads within a group are independent;
reads needing earlier results belong in subsequent calls, not the same group.

The execution server admits each entry through existing preparation,
permissions and extension hooks before launching any worker in the group, so
human approval pauses cannot leave earlier workers with stale deadlines. Each
worker receives the effective context,
prepared arguments and session snapshot over a private inherited socket. It
revalidates the audited read identity, owns its allocations and transport, and
returns a result plus the existing state delta (including ephemeral spill
handles and learning facts). Only the owner merges deltas, emits results and
runs post hooks, in input order. It never replaces the whole owner session.
No shared-runtime threading, new provider loop or persistent pool is introduced.

All started workers are collected and reaped before return. The enclosing cell
deadline and owner disconnection cancel outstanding work via retained process
ownership. Failed or uncertain workers are not replayed. Read errors occupy
their own result slots. Timeout/cancellation prevents later serial barriers.

This operates inside the normal `run_code` surface on both Responses and Chat
Completions, and the same execution server used by ACP. Multiple provider-level
Python cells remain serial: arbitrary Python scripts can mutate shared state.
Python threads must not call the synchronous tool bridge concurrently. Wasm
retains its explicit unsupported-execution-server result; there is no fallback.

## Success measure

A production-binary HTTP rendezvous must observe concurrent read effects on
both HTTP wires. Compact integration checks cover ordered result association,
write/read barriers, independent errors, malformed-batch no-effects, permission
filtering, recoverable spill handles, bounded timeout and owner-policy cancellation
of an outstanding worker before a later write.
The existing execution suite guards ordinary serial behavior. No speedup for
small local reads or paid-token/cache improvement is claimed; native process
startup can outweigh parallelism for tiny reads.
