# Review disposition

This records resolved review findings, not private account metadata or runtime
instructions. Final execution evidence is in `evidence.md`.

| Finding | Disposition |
| --- | --- |
| Claude account MCP connectors can remain despite tools/settingSources being empty | Production verified-Claude setup now includes `strictMcpConfig:true`. Earlier live routing tests did not prove exclusive tools; final parent observer must verify the production binary without injecting options. |
| ACP notifications double-count bridge tools | Verified strict Claude ignores external tool-call notifications; native bridge events are authoritative. Generic/unverified adapters preserve external tool events. |
| Two permission gates for Claude | Verified strict Claude selects bypassPermissions for every tny mode; prepared native tool rules/intercepts remain the authority. Unknown adapters receive no such guarantee. |
| Modern ACP cost object was lost | Report amount with currency and cumulative session scope; repeated updates replace totals. Unknown input/output tokens are explicit, never inferred from context occupancy. |
| ACP availability on unsupported process builds | Availability uses the process capability seam. Host tests exercise the actual wasm seam; they are not an emcc/browser build. |
| Permission waiting consumed tool deadline | Human waiting is excluded. Approval starts a fresh execution deadline; deterministic fake-clock tests cover parked and blocking prompt paths. |
| OOM cancellation could allocate/call extensions | Resource-only abort invalidates pending calls; injected pending-async failure verifies no settlement allocations or late tool completion. |
| Oversize images could be silently dropped | Captured bytes are delivered or produce an explicit bounded-result error, with byte-exact and size-limit regressions. |
| SDK command ownership/compatibility | Transactional copied argv, scoped allocation, literal empty arguments, unchanged frozen C structs and Python positional ordering. |
| New API registration and older SDK libraries | Four additions enter ABI 1.4, preserving previous symbols/layouts. SDK lookups are optional and bound to the owning library; actual ABI 1.3 Node usage and explicit ACP rejection pass. |
| Relay EOF could lose buffered output | Half-close/drain handling and regression preserve final responses. |
| Tools catalog available before a turn | Intentional: required for MCP initialization and model discovery. Actual dispatch requires the owning active session; private filesystem permissions define the same-user boundary. No change warranted. |
| Empty newline as a liveness probe | Not an ACP/MCP request or defined heartbeat. No acknowledgement is required; no change warranted. |

The completed independent read-only review supplied the permission-deadline
finding. Its earlier interrupted attempt is not successful review evidence.
Protocol limits, unverified pi adapters and native-only operations remain explicit
in the capability matrix; no blanket parity claim is made.
