# 0179 — Python code mode on pinned embedded CPython

Date: 2026-09-27
Status: accepted; release verification is recorded separately
Supersedes: the language/runtime choice in ADRs 0174 and 0178, not their tool authority or no-replay rules

## Decision and motivation

The user has chosen Python after the measured code-generation advantage in
ADR 0178 and accepts a larger executable. Ship **CPython 3.14.7, statically
embedded with a frozen bootstrap**, rather than a Python subset. There is no
external Python executable, shared libpython or standard-library-directory
requirement for code mode. Trusted extension hooks retain their separate,
optional host-Python requirement.

We built PocketPy, MicroPython, Monty and two CPython configurations. PocketPy
failed generated code containing ordinary generator expressions. A MicroPython
JSON compatibility facade removes its fixable ensure_ascii API gap: it runs all
36 old Python programs unchanged, but only 12/36 newer CPython-wording programs
on the targeted compatibility corpus. Thus the facade gap alone is not the reason
for rejection. Monty ran the old corpus and performed well in the new trial, but
its measured 6,165,632-byte embedding probe was larger than the 4,668,312-byte
minimal static CPython probe, with language-subset divergences. It did not meet
the preregistered lighter-runtime rule. These are measured configurations, not
universal lower bounds or claims about every future runtime version.

The new paired trial used the normal Codex ChatGPT login, gpt-6-luna at low effort,
108 first programs and three permitted repairs. The proposed Python wording and
the old Python wording both solved 35/36 tasks; their output tokens per solved
task were 269.37 and 276.89. Monty solved 36/36 after one repair at 289.47 tokens
per solved task. The small, deliberately compatibility-focused corpus does not
prove broad model superiority or exact equivalence. Results, failures, raw events,
protocol corrections and reproduction commands are in the
[report](../verification/python-code-mode/report.md) and
[preregistered protocol](../verification/python-code-mode/protocol.md).

## Authority and process architecture

The provider surface remains exactly run_code(code, timeout_ms?). New code is
Python, not Lua. The owning session runner still launches a fresh trusted
execution server with the existing context snapshot. That server launches a
further fresh private code-cell executable with an empty environment and a
private inherited socket. Only the trusted server executes nested tools; the
Python process does not receive the provider credentials, session authority,
workspace-selection policy or permission dispatcher.

On supported Linux x86-64/aarch64, a default-deny seccomp filter confines the
Python child after its frozen runtime initializes. On macOS the existing OS
seam uses a pure-computation sandbox profile. Unsupported platforms fail closed;
no unconfined Python fallback is provided. Removed loading/interactive builtins
are API guidance, **not the security boundary**: CPython object introspection is
not treated as safe isolation. The OS boundary denies ambient file, socket and
process creation even when native APIs are reached. This is not protection
against a compromised same-user host, kernel flaws or all interpreter defects.

Python code can compose synchronous typed operations with tools.call(name,
arguments_json), discover the permitted catalog with tools.list()/describe(),
and emit bounded output with print(). All ordinary tool preparation, allowlists,
permissions, hooks, workspace restrictions, human-wait accounting, cancellation
and completed-effect settlement remain owned by the existing server. Default
yolo/writable policy is not widened or narrowed by choosing Python.

Private cell frames use a length-prefixed transport and checked operation kind.
CALL payloads contain a fixed three-digit name byte count, the exact name and
JSON object arguments; a newline in a name cannot be reinterpreted as argument
whitespace. Embedded NUL bytes, invalid lengths, recursive run_code, invalid JSON
argument shapes and exhausted call budgets are rejected before dispatch.
A DONE frame is held until EOF **and successful child exit**. An abnormal exit,
crash or forced teardown is not relabelled successful because output arrived.

## Bounded execution and Python surface

Each cell has fresh state and a 64 MiB metered interpreter/codec heap, 64 KiB
printed output, a 256 KiB source/argument bound and at most 64 nested calls.
The default wall budget is five seconds, bounded by thirty seconds. Trusted human
permission waits retain the existing bounded extension of that budget. The
server also enforces an external wall deadline; Python instruction tracing is
not relied on to interrupt native C operations.

A fatal quota violation is process-terminal: the child sends a bounded error
and exits without running more Python except/finally/finalizer code. Ordinary
Python exceptions and nested-tool denial strings keep their documented behavior.
Completed external effects are not rolled back. A lost response is not permission
to replay a possibly executed cell.

Full Python language syntax, standard collection semantics, insertion-ordered
dicts, generators, classes, exceptions and Unicode strings are CPython's. The
standard library is deliberately **not** exposed. json is a prebound native
facade with loads and dumps, not an importable complete stdlib package. It
preserves arbitrary-sized JSON integers, booleans distinct from integers, None,
empty arrays/objects, Unicode and insertion order. The encoder supports the
ordinary ensure_ascii, indentation, separator, sort_keys, skipkeys, circular-check,
allow_nan and default-callback options. cls= and decoder hook/custom-parser
options are unsupported; decoding accepts strict JSON UTF-8 text/bytes rather
than every stdlib encoding/nonstandard extension. Error wording may differ.
JSONDecodeError exposes message/document/location information. Reentrant callback
mutations must not invalidate borrowed container references.

Available string/byte codecs follow the frozen standard-library boundary too.
UTF-8 and CPython's core builtin codecs are available, but a codec requiring an
omitted encodings module can raise LookupError: examples observed in the final
audit are cp1252, utf-8-sig, unicode_escape and idna. This is an explicit v0.24.0
packaging limitation, not a MicroPython dialect or a claim that every CPython
standard-library-backed method is present. The native JSON facade's supported
UTF-8/Unicode behavior and parser Unicode database remain as specified above.

The allocator measures the interpreter and codec allocation domain, not the
entire process RSS or trusted tool execution. Separate bounded input/output
transport buffers and CPython executable mappings exist outside that heap
counter. Do not advertise a 64 MiB whole-process-memory guarantee.

## Compatibility and packaging

No public C ABI layout changes. Shared libtny and wasm link the explicit unsupported
interpreter seam, preserving their already unsupported model-tool execution
boundary. Native test/fault executables that launch cells link the actual runtime,
not the shared-library stub. This distinction is verified by build-graph tests.
The CPython archive is keyed by build directory, target ABI and flags; glibc,
musl and incompatible build variants do not share a cached archive accidentally.
Pinned tarball digests are verified before extraction; Nix supplies the fixed
source and disables runtime downloads. Release archives/installations carry the
CPython licence and notices.

The production Lua source/link/prompt path is removed. A pinned copy remains
only under tests/bench/code_mode/lua_runtime to reproduce historical comparisons. Old stored
Lua cells are historical source, not automatically translated or replayed by a
compatibility interpreter. New run_code requests must use Python; syntax errors
are explicit. This language change belongs in the tagged release notes.

## Verification scope

Lean 4.30.0 checks eight actual C gate functions translated through typed Clang
AST into fixed-width bitvectors. It proves 36 independent specification theorems,
a helper and 17 generated no-wrap obligations; 6,070 compiled GCC/Clang UBSan
vectors cross-check the translation. The runtime-choice predicate is translated
from its actual Python AST, with thirteen specification theorems and 371 replay
vectors. Thirty-eight weakened policies or unsupported constructs are rejected
by the expected proof/translation stage. No hand-maintained parallel gate,
admitted proof or native_decide result is treated as source verification.

These proofs establish deterministic modeled gates, not the entire parent/child
implementation, CPython, JSON parser, OS isolation or stochastic model behavior.
Native integration, quota/finalizer and exact-name regressions, process-exit tests,
compiler/wasm checks, memory instrumentation and independent evidence replay are
separate obligations. See the [evidence ledger](../verification/python-code-mode/evidence.md)
for revision-bound outcomes, failures and platform limits.

### Unicode parser bootstrap
The builtin unicodedata module is statically linked and preloaded for the
CPython parser's non-ASCII identifier normalization and named Unicode escapes.
It is not a general import permission or an external stdlib dependency. A
production regression verifies Greek identifiers, NFKC normalization and named
Unicode escapes while user imports remain refused. Earlier minimal-probe sizes
remain historical; the final production artifact includes this required module.
