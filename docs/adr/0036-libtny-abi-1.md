# 0036 — libtny ABI 1 freezes a sized-prefix C contract and platform compatibility policy

Date: 2026-08-27
Status: accepted design; activation remains blocked by issues #23, #24, #27,
#29, #40, and the final issue-45 review

## Context

ABI 0 deliberately traded compatibility for rapid validation of ownership,
event ordering, cancellation, host services, custom tools, and language
schedulers. It also exposed initializers which write `sizeof(type)` without a
caller-capacity argument. That makes an in-place struct extension unsafe even
when the struct contains `struct_size`: a newer initializer could overwrite an
older caller's allocation.

ABI 1 must make binary compatibility mechanically testable without implying
that every provider, platform, or optional service is present. The first ABI 1
release is therefore a migration and packaging event, not merely changing the
integer returned by `tny_abi_version()`.

## Decision

### Activation and baseline

[`abi/baseline-v1.json`](../../abi/baseline-v1.json) is the proposed LP64,
little-endian ABI 1.0 prefix. It records every numeric constant, public struct
size/alignment/field offset, exported symbol, ELF version node, SONAME, and
Mach-O version rule. It is intentionally marked inactive. The ABI major,
SONAME/install name, export maps, and headers do not change until issues #23,
#24, #27, #29, and #40 pass, the issue-45 review accepts the generated candidate, and all
release gates below are green on both supported shared-library platforms.

The accepted candidate becomes immutable in the same commit that changes the
baseline state to active. A baseline edit after activation requires an
explicit compatibility explanation and the checker must still accept the
previous baseline as the old-consumer contract.

### Data model and calling convention

ABI 1 supports only little-endian LP64: macOS arm64 and glibc Linux x86_64 or
aarch64. Public scalars use fixed-width integer types. Public declarations do
not use C `bool`, C enums, `long`, `size_t`, bit-fields, flexible arrays, or
variadic functions. Opaque objects remain pointers to incomplete types.

Every export and callback uses `TNY_CALL`, which is the platform's ordinary C
calling convention on the supported targets, and exports use C linkage. C++
headers retain `extern "C"`; callback implementations are `noexcept` and no C++,
Objective-C, Python, or JavaScript exception may cross the callback boundary.
The `_WIN32` `__cdecl` spelling is header portability scaffolding only and does
not promise a Windows binary.

### Sized-prefix negotiation

`tny_bytes` is fixed. Every other public record begins with, or has a declared
leading header containing, `struct_size`. ABI 1 initializers take the caller's
capacity explicitly in addition to the pointer and return a status. The caller
passes `sizeof(its_header_type)`; the initializer rejects a capacity below the
baseline's `minimum_size`, zeroes no more than the supplied capacity, sets
`struct_size` to that capacity, and initializes only fields present in the
intersection of caller and library layouts.

Every ABI 1 operation:

- rejects `struct_size < minimum_size` before side effects;
- reads and writes at most `min(struct_size, sizeof(library_type))`;
- treats absent input fields as their documented zero/default value;
- never requires an unknown tail to be zero and never reads it;
- appends new fields only at or after the baseline `append_from` boundary;
- never moves, resizes, retypes, or renames a frozen field; and
- leaves the baseline reserved tail reserved forever rather than consuming it.

The explicit-capacity initializer signatures are source-breaking from ABI 0
but live only in the new ABI-1 library/header. They remove the initializer
overflow caveat instead of pretending ABI 0's one-argument initializer is
extensible. Layout suffixes (`_v0`, `_v1`) identify record/schema revisions,
not the library ABI major.

### Constants and unknown values

Existing numeric constants never change meaning or value. New values use new
numbers or previously unused mask bits; removed behavior remains a recognized
constant and returns a documented unsupported result. Unknown event kinds,
stop reasons, providers, diagnostics, and errors retain their numeric value in
language adapters and generic C handling. Unknown mask bits are ignored while
preserving known bits. Unknown permission-option bits are ignored; an unknown
permission decision supplied by a caller is rejected as invalid argument.
Unknown non-error status values are not success aliases: wrappers surface a
generic unknown-status/protocol failure while preserving the integer.

### Symbols, ELF, and Mach-O

glibc Linux ships `libtny.so.1` with SONAME `libtny.so.1` and linker symlink
`libtny.so`. Every 1.0 symbol is in `LIBTNY_1.0`. Compatible additions in a
later minor release use a new `LIBTNY_1.N` node inheriting the preceding node;
an existing symbol never moves nodes, changes type/signature, or disappears
within ABI 1.

macOS ships `libtny.1.dylib` with install name
`@rpath/libtny.1.dylib`. Its compatibility version remains `1.0.0` throughout
ABI 1. The current version is the monotonically non-decreasing numeric product
SemVer triplet; a prerelease uses the same numeric triplet as its eventual
stable release and package metadata carries the prerelease identity. The
release rejects a decreasing or unrepresentable Mach-O version.

No Windows DLL, public static archive, musl shared library, or wasm embedding
library is part of ABI 1. These targets report no shared/static capability and
receive no ABI-1 artifact. Adding one requires a platform ADR, its own consumer
matrix, and an extension of the active baseline; it is never inferred from
conditional macros in the header.

### ABI 0 migration and support windows

ABI 0 is explicitly allowed to break when ABI 1 activates. The first ABI-1
release also publishes the final `libtny.so.0` / `libtny.0.dylib` built from a
frozen ABI-0 maintenance branch. ABI-0 receives security and critical data-loss
fixes, but no features, until the later of 180 days after ABI-1 general
availability or the ABI-1 product release `1.2.0`. Both majors may be installed
side by side; pkg-config uses `libtny-0` for the legacy file during the window
and `libtny` for ABI 1. SDKs move by release line and never load either major by
an unversioned filename.

ABI-0 callers must recompile for ABI 1, pass explicit capacities to
initializers, tolerate unknown constants, and use capability snapshots for all
optional behavior. Ownership and cancellation remain as documented by the
final ABI-1 header; issue #27 closes the scheduler-cancellation contract and
issues #24/#29 decide whether fault-safe destruction or outstanding tool calls
require a handle registry before the freeze.

Within ABI 1, deprecation is documentation plus a replacement for at least 12
months and two published minor releases. Removal waits for ABI 2. Supported
ABI-1 binaries remain loadable for the entire ABI major. The project supports
the latest minor release operationally and the immediately preceding minor for
critical/security fixes; older ABI-1 binaries remain compatible but may need a
current library to receive fixes.

Security fixes ship ABI-preserving in patch releases whenever possible. If a
safe fix requires new information, it adds a sized tail field, symbol, or
capability bit in a minor release. If preserving behavior would be unsafe, the
library may fail closed or disable the affected capability in a patch release,
with an advisory and migration path. A symbol/layout break still requires ABI
2; emergency removal follows coordinated disclosure and documents why a major
bump could not be staged.

### Capabilities are runtime authority

ABI version answers only whether the calling convention, frozen prefixes, and
symbols exist. It does not imply a provider, TLS implementation, endpoint,
custom tool, host service, persistence mode, or platform package is usable.
Callers inspect the sized capability snapshot and distinguish availability,
enablement, initialization, and reachability. Missing optional behavior returns
`TNY_STATUS_UNSUPPORTED`; it is never guessed from ABI or operating system.

## Migration map

| ABI 0 | ABI 1 action |
| --- | --- |
| One-argument struct initializer | Pass pointer plus the caller's `sizeof(type)` and check status |
| Treat exact struct size as current library size | Require only the documented minimum prefix; ignore appended tail |
| Link `libtny.so.0` / `libtny.0.dylib` | Link SONAME/install-name major 1; never use an unversioned runtime lookup |
| Accept only known event/provider/stop values | Preserve an unknown numeric value through the generic representation |
| Infer functionality from a header or ABI minor | Query capabilities and use availability/enabled/initialized/reachability fields |
| ABI-0 SDK package | Upgrade to the first SDK release declaring ABI 1; mixed SDK/library majors fail before runtime creation |

## Release gates

Activation requires the automated baseline comparison and exact export maps;
old ABI-1.0 C and C++ source fixtures compiled against the minimum header; the
same old binaries run against the current library on macOS arm64 and both glibc
Linux architectures; current source compiled against both the minimum and
current headers; deliberate constant, layout, export, SONAME, install-name,
and symbol-node drift failures; and complete #40 conformance.

The existing ASan/UBSan, Linux x86_64 TSan, fault injection, fuzz, mutation,
TLS, size, startup, SDK, and package matrices remain hard gates. Binary fixtures
are release artifacts with SHA-256 manifests, compiler identity, target triple,
and minimum OS/glibc metadata; they are never regenerated inside the
compatibility job. Source fixtures are checked in and compile with `-std=c11
-Wall -Wextra -Werror` plus a C++17 `-Werror` header consumer.

This ADR supersedes ADR 0023 only for compatibility and packaging policy after
ABI 1 activation. Until then, ADRs 0023, 0032, 0033, 0035 and the live ABI-0
header remain authoritative.

## Consequences

ABI 1 has a conservative native surface with measurable compatibility and a
finite ABI-0 migration window. The explicit-capacity source migration is
intentional: it pays the experimental break once so future tail growth is safe.
New functionality may ship without an ABI major only when both the baseline
checker and runtime capability model say it is compatible.
