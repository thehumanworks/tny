# 0174 — Isolated execution server and code-only agent tools

Date: 2026-09-25
Status: accepted; verification status is tracked separately

## Context

Advertising every native tool couples provider calls to the harness dispatcher,
expands provider-specific policy paths and encourages callers to compose many
independent requests. Execution authority must remain with the owning runtime
while bounded code composes permitted operations in a separate process.

## Decision

Native Chat Completions, Responses and the ACP MCP bridge advertise exactly one
function, `run_code`, with required string `code` and optional integer
`timeout_ms`. Other provider-level tool names are rejected before execution.
Hosted provider search is no longer an alternate tool surface; the ordinary
`web_search` operation is available through code and retains its independent
credential selection.

Each invocation starts the matching tny executable as a fresh private
`--exec-server` child over an inherited private bidirectional Unix socket. The private entry point
is not a public server, network listener or user-selected command. The caller
supplies a trusted context snapshot; code cannot choose session identity,
workspace policy, tool profile, provider credentials or an alternate executable.
The executor owns the Lua state and executes nested operations using that
snapshot. Owner-only decisions and callbacks cross checked reverse RPC.
Unsupported authority or callback reconstruction fails explicitly.

The child loads pinned vendored Lua 5.4.9 with only selected base,
string, table, math and UTF-8 functionality. It has no `io`, `os`, `package`,
`debug`, `require`, `dofile`, `loadfile` or `load`. Code uses:

```lua
local catalog = tools.list() -- JSON describing permitted tools
local schema = tools.describe("read_file") -- one tool's JSON schema
local result = tools.call("read_file", '{"path":"README.md"}')
print(result)
local arguments = json.encode({path = "README.md"})
print(tools.call("read_file", arguments))
```

`tools.call` returns a string; JSON conversion is explicit. Nested calls retain
prepared arguments, permissions, path constraints, hooks and tool-profile
filtering. Nested `run_code` is refused. During active code execution, runner
control requests to `image_attach`/`image_preview` are refused before file
access regardless of role. Existing role restrictions remain unchanged and
already reject owner image controls; the active guard covers otherwise allowed
requests without relying on the self-declared role. Manual frontend attachment
waits until the active code cell ends. Call the corresponding typed nested operation so the execution server
reads the bytes. Intercepted simple first-party commands use that server path. Ordinary defaults remain yolo and
writable; code does not create a new permission grant.

Each cell has a fresh state, a 16 MiB Lua allocation limit, a 64 KiB printed
output limit and at most 64 nested calls. The default deadline is 5 seconds;
explicit deadlines are integers from 1 through 30,000 milliseconds. A trusted
owner permission/question wait pauses that code budget for at most five minutes.
Foreground owner loss denies an outstanding permission; explicit background
permissions can remain pending for reattachment. A reply must match its
outstanding permission ID. Protocol
frames have a separate 8 MiB bound. Cancellation, timeouts, invalid protocol,
child crashes and EOF settle as errors. Shell execution uses the private
`--exec-command` guardian, which observes its execution owner and terminates the
owned shell tree if that owner dies. Guardian reaping can finish shortly after
the parent's error response; tests separately verify bounded disappearance and
absence of later effects. Captured image delivery through this boundary has a
4 MiB admission limit so encoded state fits inside the protocol frame bound.
A possibly executed operation is never
replayed merely because its result was lost.

All ACP prompts require the verified Claude adapter version 0.75.1, private
local scratch cwd and explicit tools-only metadata. Unknown adapters cannot
silently retain an unmediated built-in execution path. Model catalog operations
and platform errors remain distinct from prompt admission.

## Platform and compatibility consequences

Native agent calls now require the execution child even for ephemeral and
in-process session modes. The detached session runner continues to own session
lifecycle; the execution server is a separate, shorter-lived boundary.
MCP clients and connections are cell-scoped. Dependent stateful MCP calls work
within one cell; connection continuity across separate cells is not preserved.
Standalone CLI toolkit operations are not provider tool calls and keep their
existing interface. Public C ABI layouts do not change. Library-hosted agent tool execution is
currently rejected explicitly, even if a matching CLI is installed; there is
no configured executable escape hatch. The native CLI's private prompt
optimisation context is the sole internal exception. Registered custom tools
or host-service callbacks also cause execution to fail closed because their
pointers cannot be reconstructed in a fresh executable. Standalone SDK image, speech and dictation service
operations retain their separate interfaces. Public SDK optimisation uses an
agent loop internally and has no trusted execution-server launcher; it now
returns `TNY_STATUS_UNSUPPORTED` before I/O (cancellation-before-run takes
precedence). It cannot treat a failed Python/Node self-exec as successful
workspace exploration. No CLI lookup or direct-execution fallback is added.

Wasm cannot spawn this native process and returns a clean unsupported-execution
error. It does not silently fall back to direct tools. This is an explicit
platform limitation rather than native/wasm execution parity.

## Bounded operating envelope

The 8 MiB frame limit applies to the **whole initial context snapshot**, including
stored session history, code, permission state and any encoded captures. A
session can therefore be valid for storage yet too large for tool execution.
Compaction currently keeps historical messages and does not remove this ceiling;
start a new session with a concise handoff when the snapshot limit is reached.
The error is explicit and does not fall back to in-harness execution. A future
bounded history projection or chunked snapshot protocol would be a separate change.

Native execution also requires the existing generation-safe process-tree host
seam. Supported Linux hosts need working pidfd operations, and supported macOS
hosts need the process-identity signal API used by that seam. Other BSDs,
MSYS2/Cygwin and hosts that deny those operations may still build or run non-tool
commands but cannot run model tools through this server. They receive the same
explicit unsupported-execution error; successful compilation is not runtime
capability proof. Wasm remains unsupported as described above.

## Verification

See the [contract](../verification/execution-code-mode/contract.md) and
[evidence](../verification/execution-code-mode/evidence.md). Pure transition
proofs establish only their modeled protocol obligations. Production subprocess
checks, schema checks, policy tests, crash/no-replay tests and required build
and quality gates are separate evidence. No model-quality or performance gain
is claimed without a measured comparison.
