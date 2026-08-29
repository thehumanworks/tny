# 22 — Extract `run_one_tool` from OpenAI `run_tools`

Medium. From the complexity review. Independent of 20–21.

`src/backends/openai/openai.c` `run_tools` (~232 lines) repeats the same
“complete + free five pointers + bump index” sequence on deny,
prepare-fail, save-fail, perm-deny, allow-once, and park. That CC is
control-flow duplication, not a rich state machine.

## Work

- Extract `static int run_one_tool(oa_impl *o, oa_call *pc, int index)`
  returning `0` continue, `1` parked (wait), `<0` fatal.
- Keep `while (o->tool_index < o->calls.n)` as a short driver
  (cancelled calls, index bump, park/fatal).
- Permission and extension policy stay in one place instead of four
  near-copies of `complete_tool` + frees.
- Do not change wire behavior: existing `tests/test_openai.c` and
  `tests/integration/test_openai.py` (fragmented / parallel tool calls,
  deny, park) remain the contract.

## Acceptance

- OpenAI mock integration still passes, including parallel tool-call
  assembly and permission deny/allow-once.
- `run_tools` is a small loop; a missed `free` on one deny path is no
  longer duplicated four times.
