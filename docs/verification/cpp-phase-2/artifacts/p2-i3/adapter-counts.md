# Supplemental adapter measurement

The C fixture in `adapter-counts.c.txt` was compiled with Makefile `CC` and
`FAULT_PIC_CFLAGS`, linked with `CXX` against every `FAULT_PIC_OBJS` object,
and run as `build/p2-i3-adapter-counts`. All commands exited 0. These are the
real fault-library objects, including the C allocator override; no fake cancel
callback was substituted. It started a real local Cursor callback HTTP server
and populated the real ACP permission/read buffers and pipe descriptors.

Output: `Cursor callback teardown and ACP pending-permission teardown: 0 allocations each`.

This verifies the local resource-release paths, not a live Cursor bridge,
remote ACP service, or their reconnect/resume protocols. Native real-library
turn/retry proof is provided separately by the strict Responses fixture.
