# ADR 0130: Windows stream decoder LTO exemption

Date: 2026-09-17. Status at creation: accepted.

Hosted Windows run 35228495551, job 105226101765, at PR148 head 2ac54a7
passes the prior Responses/runner failures and reports the same GCC 15.3.0
`binds_to_current_def_p` internal compiler error at stream_decode.cpp:55.

Extend ADR0122/0128/0129's native-Windows/GCC release LTO exemption to that
translation unit. Preserve strict warnings, exception handling, size optimization,
all other object graphs and final-link LTO. The alternative of rewriting correct
ownership code to perturb GCC is harder to maintain. Do not exempt unobserved
translation units; any further exact compiler failures need recorded evidence.

The actual Makefile regression checks the affected object alongside Responses
and runner on Windows and other platforms. Hosted Windows runtime and artifact
size checks remain required. No performance improvement is claimed.
