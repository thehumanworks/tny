# ADR 0128: Native Windows GCC Responses LTO exemption

Date: 2026-09-17. Status: accepted for the issue #144 post-review correction.

## Evidence

At candidate `2d710b60294c8bd5fa0657182b81984d4e13b1a4`, hosted
`windows-x86_64` failed at the GCC 15.3.0 release link, during GIMPLE `tailr`
in `responses.cpp::translate_input`, with `binds_to_current_def_p` at
`symtab.cc:2598`. See task-cache `ci-windows-x86_64-first.log:601-615`.
This is a compiler internal error, not a failed runtime assertion.

MSYS2 reports [11726](https://github.com/msys2/MINGW-packages/issues/11726)
and [25202](https://github.com/msys2/MINGW-packages/issues/25202) describe the
same assertion family with size optimization and LTO. The latter reports
GCC 15.2.0. These support a narrow workaround; they do not establish that this
candidate has been successfully rebuilt on Windows.

## Decision

Reuse ADR 0122's existing native-Windows/GCC boundary: when `WINDOWS=1` and
`REL_LTO=-flto=auto`, add only `src/backends/openai/responses.cpp` to the
existing LTO exemption list (which already contains `src/core/jobs.cpp`).
Its native release object compiles with `-fno-lto`, retaining `-Os`, exceptions,
strict warnings and all other flags. The link and all previously nonexempt
translation units retain LTO. No new exemption applies to Clang, Linux,
Darwin, PIC/shared, fault, sanitizer or wasm object graphs. No warning or
safety check is disabled. Public ABI/export files remain unchanged.

Prefer this existing build mechanism over restructuring correct JSON ownership
code to perturb an optimizer crash. The exemption can be removed after a fixed
Windows compiler passes the original LTO build; it is not a performance claim.
ADR 0122 and committed ADR 0127 remain byte-for-byte unchanged.

## Verification boundary

A focused test uses actual Makefile recipes in an isolated fixture to verify
Windows/non-Windows release flags, retained LTO on other objects and link, and
unaffected debug/fault graphs. Local native request/lifecycle/mutation and
changed-source quality gates are recorded in the mutable issue #144 evidence.
The supervisor must rerun Windows CI on frozen corrected source. Local dry-run
flag evidence is not Windows compile or runtime proof.
