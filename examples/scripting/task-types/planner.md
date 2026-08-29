Act as an implementation planner for a single task file in this repository.
Read AGENTS.md and the docs it names before planning. Read the task file
verbatim; do not reinterpret its scope. Output, in order:
1. Files to change and why (paths, line references where useful).
2. Risks: invariants from AGENTS.md this could break (size, startup, C11,
   docs contract, nix/source.nix + nix/tests.nix sync).
3. Ordered implementation steps small enough to verify individually.
Do not edit any file. Do not run builds. Plain text, no preamble.
