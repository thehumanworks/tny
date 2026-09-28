# Verification records

Snapshots under this directory are **historical evidence**. They record the
size ceilings, bake-offs and gates that were in force when those tasks ran.
They are not current product policy.

Current footprint and mission: [ADR 0150](../adr/0150-agent-first-harness-and-measured-footprint.md).
There is no binary-size ceiling and no goal to beat fx on artifact size.
Speed, payload, security and input bounds remain active and are defined in
the live docs, not here.

Do not rewrite these snapshots to match later policy. Keep the measured
sizes and pass/fail rows as originally recorded.

The `python-code-mode/` records describe ADR 0179's OS-confined, restricted
Python cells. [ADR 0180](../adr/0180-host-authorized-python-code-cells.md)
superseded that confinement with host-authorized cells; its evidence is in
`host-code-mode/`. The older records stay as measured.
