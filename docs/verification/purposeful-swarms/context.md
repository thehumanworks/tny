# Purposeful swarm context inheritance

File-defined workers inherit the lead's exact accepted instruction context.
Before durable launch, the jobs layer captures the existing `tny_ctx` fields
for the explicit system prompt, resolved task name/source/body/digest,
`context_enabled`, and the ready project-instruction text/path/digest snapshot.
Workers restore those bytes before provider resolution and system construction;
they do not reread task or project files for inherited context.

The snapshot is a bounded `context.json` sidecar in the private job directory.
It is written atomically with mode `0600`, contains no provider, chat, image, or
credential mapping, and carries a SHA-256 integrity digest over its context-only
payload. Its path—not its contents—is passed in child argv. Instruction bodies
remain absent from argv, environment values, `job.json`, and status output. A
missing, oversized, malformed, out-of-directory, or digest-mismatched sidecar
stops the child before provider I/O. Retry reuses and revalidates the original
immutable sidecar, so checkpoint/rebind cannot substitute ambient files.

Authority remains separate: permission mode and maximum steps use the existing
child launch ceilings, and workspace read-only state continues through the
existing team environment/workspace policy. Restoring context changes none of
those fields.

`tests/integration/test_swarm_context.py` covers explicit system/task text,
context-disabled inheritance, an `AGENTS.md` change after capture, missing and
corrupt sidecars, the capture bound, and absence of fixture credentials or
instruction bodies from public/launch surfaces. The fixture uses only synthetic
credentials and the existing local mock provider.
