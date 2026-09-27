# Native JSON codec correctness review

Baseline88e3d4c. The scoped Opus/high reviewer identified and patched unsafe list
mutation across default callbacks, malformed dictionary subclass items, a leaked
and unchecked globals-name reference, evaluation after compilation exhausted the
heap, NUL truncation in tool discovery, and specific JSON error/separator API
mismatches. The worker was terminated by signal9 before completing its test ledger.
Its patch is checkpointed for supervisor verification, not declared release-ready.

Independent negative-before checks through actual isolated production cells
reproduced both container-related crashes and the truncated-name discovery error;
a normal signed-integer/null/empty-shape control succeeded. The supervisor must
rerun those exact controls on the integrated fix, add persistent regression cases,
and retain all existing runtime/integration checks before release acceptance.
