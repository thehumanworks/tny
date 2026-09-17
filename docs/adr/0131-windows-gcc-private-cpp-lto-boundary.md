# ADR 0131: Consistent native C++ release graph on Windows GCC

Date: 2026-09-17. Status at creation: accepted.
Supersedes the per-translation-unit Windows GCC LTO policy in ADR0122,
ADR0128, ADR0129 and ADR0130; those evidence records remain unchanged.

## Evidence

PR148 Windows runs 35213882652 and 35228495551 show GCC 15.3.0 PE
`binds_to_current_def_p` ICEs across multiple ownership modules. After scoped
exemptions, run 35229000001/job 105228193383 at eda78cf fails linking checkpoint
restore_document/recover against a missing compiler-generated
`std::unique_ptr<yyjson_doc, tny::document_deleter>` destructor `.lto_priv.0`.
The exact template-binding mechanism remains an inference, but the failure
crosses the mixed native/LTO C++ graph. Independent integration review supports
a coherent language boundary instead of continuing filename exemptions.

## Decision

On native Windows with GCC-style `-flto=auto`, compile every private C++ release
translation unit with `-fno-lto`. C and vendored objects and the final link retain
LTO. Preserve -Os, strict warnings, exceptions and all other flags. Windows Clang,
non-Windows, debug/PIC/fault/sanitizer and wasm graphs are unchanged. Keep the
explicit `LTO_EXEMPT_CPP=` override for testing a fixed compiler with `-B`.

## Alternatives and consequences

Another checkpoint-only exemption may move the template failure to another
module. Rewriting correct ownership or changing optimization globally would
couple product code to a compiler defect. The consistent private C++ boundary is
simpler and covers newly added ownership modules on this one affected toolchain.
Windows loses C++ cross-unit optimization; no speed claim is made. All artifacts
must remain below 6,000,000 bytes. No diagnostic or behavioral check is disabled.

## Verification

Maintained Makefile tests enumerate every C++ release object under Windows GCC,
its explicit override, Windows Clang and Linux GCC. Mixed build fixtures check
C++ exception behavior, retained C/final-link LTO, and unchanged other graphs.
Hosted Windows compilation, unit/integration checks and artifact size must pass.
See ../verification/merge-148/evidence.md for actual results.
