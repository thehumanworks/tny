# ADR 0123: Reconcile the two C++ migration histories

Date: 2026-09-16. Status: accepted.

The main and feature branches independently completed the same #137–#139
implementation series from c6d938a. Main reached ca1e98b; feature additionally
contains newer parser cancellation/fragmentation/OOM fixes, real parser/event
benchmarks, portable ownership fixtures and published CI/glibc compatibility
repairs. Keeping both implementation families would create duplicate symbols,
competing lifetime abstractions and unmaintainable build modes.

Reconcile main as a feature-branch parent while retaining the newer feature
implementation: util/ownership.hpp, json/ownership.hpp, stream_decode.cpp,
owned_event.cpp, custom_tools.cpp and util/resources.hpp. Do not reintroduce
the alternate src/cpp owner family or events.cpp parser. Runtime/provider
settlement, async leases, runner/job semantics and main's in-place durable
cleanup/checkpoint fixes are already incorporated. Audit found a missing
Cursor temporary auth wipe; restore it and extend the secret-buffer check.
Run the integrated source through ownership, fault, mutation, ABI and platform
gates rather than rely on either branch's historical passing tests.

Import main-only historical verification artifacts and finalized ADR files
without changing their bytes. Both branches allocated some ADR serials before
integration; preserve their distinct published filenames as historical
records, not competing current policies. The sole colliding filename is
0116-runtime-event-and-async-ownership.md: keep main's published bytes there,
and retain the feature version verbatim under the continuation's
historical-feature-adr0116.md. Current architecture and the continuation
contract describe the integrated state. New decisions use unique serials.

This is not a merge into main or a force-push. The feature PR becomes a normal
reviewable descendant of main. The public headers, ABI definitions and
unrelated worktrees remain unchanged. Only the newer explicit six-MB policy
(ADR0121) supersedes historical artifact constraints; performance and memory
safety requirements remain active.
