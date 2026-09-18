Implement issue #159 in isolated worktree feat/swarm-context, base
89bcd5918da0e225e1806813a206d007daafac0a. Read AGENTS and mandatory docs first.
Read the full issue via gh issue view 159 --repo thehumanworks/tny; read lead's
contract /Users/tomas/.tny/worktrees/5fe4c489fba3f2e0/docs/verification/swarm-delivery/contract.md.
You own sdk/python/src/tny/workflow.py and related exports/tests; TypeScript
workflow implementation/declarations/tests; shell/tny-workflows.sh ONLY for
context conformance; docs/workflows.md context sections; new ADR 0137.
Do not modify core/native code, build/Nix files, any other ADR, or delivery ledger.
Implement lazy composition after admission, selective explicit summary/JSON
fields/artifact references with bounded access and provenance, whole composed
input bound; preserve current default byte-bound semantics, edge order,
no-context, cancellation and failure propagation. Original artifacts remain
available; dependency text stays untrusted. No paid implicit summarization.
Also implement #158 SDK task usage retention/aggregation if cleanly independent:
unknown != zero, don't double-count reused artifacts; document actual coverage.
Add Python and JS 32 consumers x256KiB barrier regression; measure baseline and
candidate peak memory, composed bytes and latency using an isolated baseline or
loading baseline module, no network. Install/build native SDK dependencies to
run tests, do not count import errors as passes. Run SDK/shell applicable tests
and quality. Lead owns full gates after integration and build/Nix registration.
Independent design reviewer has completed; jobs durable mode will remain
separate from these ephemeral SDK APIs. Do not add a competing persistence layer.
Commit coherent work. No push/PR/issue closure/merge or further delegation.
Return commit SHAs, files, criterion mapping, actual commands/exits, benchmark
results, required build/Nix wiring, and honest remaining gaps. Implement complete
#159, not just the allocation fix. Preserve all existing work.
