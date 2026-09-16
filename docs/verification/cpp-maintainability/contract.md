# C++ maintainability continuation — 2026-09-16

User goal: continue #137 -> #138 -> #139 ownership migration for maintainability,
extensibility, reliable error handling and high performance. Binary size is no
longer an optimization objective provided the tny artifact stays below 6 MB.
Interpret MB as decimal: artifact bytes must be < 6,000,000, including wasm
with its glue. Runtime dependencies are reported separately. Existing latency,
throughput, memory, ABI and safety requirements remain unchanged. Historical
ADRs and initial contracts retain their original text; this user amendment
supersedes their old size ceilings and byte-saving incentives.

Start: clean feature branch feat/cpp-ownership-137-139 at 650ef86. PR #140
already exists. Remote main advanced to the alternate migration ca1e98b;
reconcile that history on the feature branch without losing either branch's
correctness fixes. Never modify/push remote main, force-push, or touch unrelated
worktrees. Update the existing PR rather than create a duplicate.

## Done criteria

1. Ownership-bearing code stays private C++20; C ABI and native OS seams remain
   stable. No mechanical repository-wide renaming. Prefer simple RAII with
   explicit cancellation/persistence protocols over byte-saving cleverness.
2. One enforced <6,000,000-byte ceiling in native, static, wasm, CI/release and
   installed-package checks. Boundary tests fail at the ceiling. Preserve
   measured startup, parser/event throughput and memory regression gates.
3. Resolve current hosted failures: GCC aggregate fixture initialization,
   obsolete MSYS package assertion, formatting and Windows GCC LTO crash.
   Mutations must generate valid behavioral tests; empty/all-invalid selectors
   fail, not silently succeed. No warning/test suppression.
4. Current source passes native quality/full tests, ownership, allocation fault,
   sanitizer, ABI/SDK/leak/fuzz/mutation checks. Reconcile hosted CI/Nix/SDK on
   the published candidate; skips are not claimed as runtime proof.
5. One fresh read-only Fable medium-effort review of this continuation; record
   findings and dispositions. Earlier unavailable review output is not reused
   as approval. Implementation helpers may not review or spawn subagents.
6. Source-bound evidence, performance/accounting, committed/pushed feature SHA,
   mergeable PR #140 with accurate checks. No main merge or issue closure.
