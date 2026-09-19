# ADR 0154: Automatic workflow learning is on by default

- Status: accepted
- Date: 2026-09-19
- Extends [ADR 0153](0153-bounded-instruction-evolution.md)

## Context

The user requires self-improvement during normal agent work, without selecting a
preset or launching an experiment. ADR 0153's optional controller alone does not
meet that requirement. SoL-Pi's opt-in distribution is inspiration, not authority
over the user's requested default.

Use the research distinction established in ADR 0153: empirically improve a
bounded part of the harness, retain evidence, and do not confuse fixture results
with general model improvement. ACE motivates revisable context from execution;
GEPA motivates feedback-driven policy selection; SoL-Pi motivates constrained
harness improvement that preserves useful work. This is not a reproduction of
those full algorithms or Darwin Gödel Machine's code-level self-modification.

## Decision

Add a native C11 **default-on recovery-policy learner** to ordinary CLI and TUI
turns. It observes work already authorized by the user. It makes no additional
provider calls, starts no Python process, schedules no extra task, and executes
no command on its own. The optional task-instruction experiment controller stays
available for broader, explicitly configured research.

The initial policy space is deliberately narrow: useful inspection of current
file content before retrying an exact edit. Learning is real and automatic, but
not open-ended. An empty workspace has no promoted empirical advice.

### Execution facts, not prose

A qualifying episode is:

1. An actual exact edit returns `NOT_FOUND` or `AMBIGUOUS`.
2. A successful, nonempty read inspects the same target.
3. A related exact-edit retry commits, or fails again.

The adapters report typed outcomes before rendering or extension replacement.
`read_file` content that starts with `error:` is still a successful read. A
terminal result that lacks an `error:` prefix is not automatically a success.
The intercepted `tny edit` adapter reports its real editor status.

Targets and replacement intent are hashed transiently in RAM. Changed target or
replacement intent, unsupported operations, failed reads, and uncertain results
cannot complete an episode. Supported terminal diagnostics are single simple
`cat FILE` or `sed -n RANGEp FILE` commands with nonempty output, actual exit zero,
no timeout/cancellation, and a matching target. Shell chains, substitutions,
background acknowledgements and arbitrary successful commands are not evidence.
No permission decision is inferred or overridden by learning.

### Bounded revision and application

- After two successful recovery episodes, a policy becomes eligible only while
  `successes > 2 * failures`. Attributable failed retries can demote it.
- At most eight intervening diagnostic events are considered. Unknown operations
  reset the episode. Counters age at a bounded total of 1,000.
- Eligible policies render a fixed advisory block of at most 1 KiB into the next
  ordinary request, before task presets and explicit system-prompt additions.
- Templates and counters are the only rendered data. Raw tool output, commands,
  arguments and filenames never become durable instructions.
- User instructions, task constraints and permissions always take precedence.
  A recovery sequence is a temporal observation, not proof of causality.

Evidence is workspace-scoped at `~/.tny/learning/<workspace-hash>.json`. It holds
bounded counters and the most recent source session ID for each policy. Existing
sessions retain the original detailed tool history. This is not a full immutable
experiment archive; use ADR 0153's controller for that purpose.

Persistence uses private files, no-follow checks, atomic replacement, and
nonblocking lock/reload/merge updates. Corrupt or unsupported data is ignored and
preserved. Contention retains bounded local deltas for the next opportunity;
normal completion supplies a flush opportunity, and a reused backend retains
pending deltas across turns of the same workspace. Process exit/restart while
the store remains unavailable can lose unflushed observations. State I/O does
not acquire tool or model authority.

### Defaults and boundaries

`self_improve` defaults to true. `TNY_SELF_IMPROVE=0` and the leading
`--no-self-improve` flag disable observation, injection and learner-state I/O.
The flag survives runner checkpoints, subagent launch plans, nested terminal
children and durable jobs. `status` and `doctor` expose the effective setting.

Normal native CLI/TUI turns load and update the workspace store. Ephemeral,
libtny and wasm contexts use only current-turn memory. They do not read or write
ambient learner state. Prompt optimisation is excluded. SSH operations are not
classified in this first version and never contaminate the local workspace
store. No public C ABI layout changes, TUI framework or provider implementation
is introduced. The platform-specific storage fallback lives at the OS seam.

## Verification and claim boundary

Real native mock-provider tests cover both wires, all tool profiles, the TUI,
new sessions, opt-out, ephemeral isolation, matching target/intent, negative
feedback, and displayed-text traps. Unit/fault checks cover persistence,
concurrent merging, corruption, symlinks, bounded state and inheritance.

A replay benchmark compares cold/default, learned/default, explicit disable and
an optional pre-change binary on identical local file tasks. It records actual
native tool calls and mock HTTP requests with byte-exact file checks. The mock
policy is intentionally responsive to learned advice. This proves default
execution and the measured replay effect, not that a real model will follow the
advice or improve on general coding tasks. No token, latency or dollar gains are
claimed from this fixture.
