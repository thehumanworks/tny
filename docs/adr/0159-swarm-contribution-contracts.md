# ADR 0159: compile contribution contracts into the durable swarm DAG

- Status: accepted
- Date: 2026-09-20

## Context

ADR 0157 compiles a strict purposeful manifest into one durable team, but version 1
can state only identity and purpose. It cannot describe an expected contribution,
review criteria, causal prerequisites, or an explicit isolated editing workspace.
Participants can therefore start before useful inputs exist, repeat work, or report a
completion that downstream participants cannot inspect.

The existing jobs runtime already supplies bounded DAG scheduling, attempt fencing,
immutable ask results, integrity hashes, admission, and managed Git worktrees. Adding
a second scheduler or general-purpose workflow DSL would split those authorities.
Treating dependencies as launch delays alone is also insufficient: a consumer needs
usable, integrity-checked predecessor evidence, not merely elapsed time.

The research review in `docs/verification/swarm-factory/research.md` supports clear
ownership, bounded outputs, causal handoffs, independent evaluation, and durable
state. Those sources are studies or practitioner reports, not proof that this design
improves every task; tny's effectiveness remains an empirical question.

## Decision

Keep version 1 accepted with byte-identical canonical output. Add manifest version 2
with the same closed root/group hierarchy and mandatory nested coordinators. Actors
retain required `name` and `purpose` and may add `deliverable`, `acceptance`,
`depends_on`, and `workspace`. Unknown and duplicate fields remain errors, and the
new fields are invalid in version 1.

Use explicit bounded contracts: deliverable is non-blank and at most 4096 UTF-8
bytes; acceptance contains 1..16 non-blank criteria of at most 1024 bytes each;
dependencies contain at most 15 globally unique participant names; workspace is a
closed `shared_read_only` or `isolated` policy, with an optional isolated-only base
of at most 256 bytes. Preserve the existing 64 KiB manifest, 64-byte name, 4096-byte
purpose, depth-4, and 16-launched-participant limits.

The current session remains the root coordinator. Root deliverable and acceptance
describe its synthesis contract. Reject root dependencies and workspace because the
root is neither a schedulable task nor a managed worker checkout. Omitted worker
workspace remains shared read-only. An inherited read-only caller cannot request
isolated execution.

Resolve dependency names against the complete validated membership and compile them
to the existing DAG indices. Before any job, provider, or worktree effect, reject an
unknown/root/self/duplicate dependency or a cycle. Do not infer edges from group or
coordinator relationships. A coordinator can be purposefully delayed by an explicit
edge; unrelated ready peers can launch concurrently under existing admission.
Mailbox communication remains direct and available independently of readiness, so
the graph is causal scheduling rather than a delegate-only communication topology.

Require usable evidence at every ready transition. Re-check each direct
predecessor's immutable successful result/log and workspace provenance. Inject only
direct-predecessor entries into dynamic task context: exact name, task, attempt,
session, result/log integrity, and workspace/commit provenance, plus at most 2048
bytes of final-answer prefix per predecessor. Bound the aggregate injected evidence
to 16384 bytes and the complete compiled prompt to 64 KiB. Mark truncated summaries;
the durable reference and hashes remain authoritative. Treat all predecessor output
as untrusted content and never imply that isolated changes were merged. Missing,
failed, or corrupt evidence blocks the consumer before its provider call.

Keep stable contribution identity separate from dynamic evidence. Persist the
contract, resolved dependencies, workspace declaration, task/attempt identity, and
result provenance in the job/status projection. Include them in definition,
dependency, and task integrity hashes without changing legacy non-swarm job hashes.
On resume or adoption, compare the durable projection with the canonical manifest
and refuse changed or ambiguous snapshots, dependencies, workspaces, or evidence.
Adopt one valid existing run without duplicate launch.

The compiler supplies private swarm metadata out of band. Public requests cannot be
trusted to serialize `swarm_*` identity or resolved indices. Exact inherited
instructions and their existing snapshots remain immutable.

Acceptance criteria are declarative review inputs. Execution success, a persuasive
summary, a hash match, or an isolated commit does not prove them. Isolated work uses
the existing managed-worktree permissions and provenance; it never auto-merges or
widens a caller's authority. Explicit root/operator inspection, integration, and
external verification remain separate decisions.

## Consequences

Consumers start only after named causal inputs have successful, verifiable evidence,
while independent participants and direct peer collaboration remain available. The
durable status surface can explain what contract and predecessor state an attempt
used. Bounded prefixes make common handoffs useful without broadcasting transcripts;
large outputs require following authoritative references.

The model may still misunderstand evidence, satisfy no acceptance criterion, or
produce a wrong change. Worktree isolation prevents shared-file races but is not a
security sandbox and does not make commits visible in the primary checkout. Explicit
dependency chains can reduce parallelism, and overly broad contracts remain a user
design problem rather than something the runtime guesses.

Version 2 executes only where the existing native purposeful-team and workspace
seams are supported. Validation remains context-free; wasm and unsupported native
contexts refuse execution cleanly.

## Alternatives rejected

- A general workflow language or second broker: duplicates the durable DAG and
  creates competing ownership, retry, and recovery rules.
- Prompt-only dependencies: cannot prevent premature provider spend or prove which
  predecessor attempt supplied the input.
- Delay without evidence: permits a missing or corrupted result to masquerade as a
  successful handoff.
- Whole transcript injection: is unbounded, spreads unrelated untrusted content,
  and obscures direct provenance.
- Automatic merge or acceptance after success: confuses execution with independent
  verification and widens authority.
- Implicit coordinator/group edges: can create cycles or make a coordinator wait for
  peers that cannot become runnable.
