# ADR 0083: Optimiser timeout overrides without a step cap

Status: accepted. Supersedes the step/deadline limits in ADR 0082.

## Decision

Prompt exploration can need more than 12 steps or two minutes. The optimiser
sets its own max_steps to zero and does not inherit parent step limits.
It defaults to a 300-second deadline, checked between engine steps.

Resolve timeout seconds from the subcommand --optimise-timeout option,
TNY_OPTIMISE_TIMEOUT, project .tny.json optimise.timeout_seconds, user
settings optimise.timeout_seconds, then 300. Use integer seconds from
1 to 86400; reject invalid selected values before provider contact.
Project configuration controls only this duration, not tool authority.

Cancellation, read-only tool enforcement, and output bounds are unchanged.
The same service implements this behavior for native and wasm callers.

## Verification

The existing optimiser integration suite covers all timeout sources,
precedence, rejection, actual short deadlines, longer overrides, and
completion after 20 tool calls despite a parent one-step limit.
