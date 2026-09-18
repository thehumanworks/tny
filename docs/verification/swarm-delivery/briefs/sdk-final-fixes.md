Continue your SDK hardening branch. Independent follow-up review b739d967df11861c
confirms numeric policy/bounds/ordinary failure fixes, but reproduced two Python
bugs at 9cf38cf. Own only Python SDK source/tests, docs/workflows.md and ADR0137
addendum. Do not touch native/JS/build/Nix or delegate/publish.
1. Actual AsyncSession + real generator: callback raises or cancellation while
awaiting callback. Native runner async-for does not close session.run generator
BEFORE leaving session context. Session closes first; finalizer skips drain.
Probe synchronous events usage7 then usage9: sequence usage7,close, partial7;
expected usage7,cancel,usage9,close,partial9. Explicitly finalize/drain stream while
session is open, including callback/permission callback failures/cancellation.
Add tests using actual AsyncSession wrapper with fake sync/native event transport,
not direct _cancel_and_drain or fake cleanup returning last_usage.
2. _detach_error follows cause/context but not BaseExceptionGroup.exceptions.
32 failed consumers can retain all 8,394,816 prompt bytes through child tracebacks.
Traverse nested exception groups safely, preserve group topology and exception
messages/types, clear child frames/chains too. Respect supported Python versions;
no unbounded traversal of arbitrary application attributes. Add nested/mixed group
weakref+traceback assertions and ordinary regression test. Reviewer already reran
benchmark baseline/candidate ordinary failures successfully; preserve that win.
Run Python workflow + full native SDK tests, strict typing/Ruff. Commit and return
exact SHA/checks and any remaining risk. Lead integrates final gates. No live calls.
