# Direct tny live acceptance evidence

## Final delivered-source rerun — PASS

Checked 2026-09-13T09:17:56+00:00. All three required scenarios passed on release SHA-256
`a25ffd6f80bd9d78657b93ef1ea5759cf4804500deb9e78f8123192987399346`: native Codex search/follow-up `d501d8e00afe0b10`,
Grok `grok-4.6` terminal-profile DuckDuckGo search/follow-up `7708562db2d53211`,
and Grok `grok-4.6` lifecycle `d980952edff60f1b` (runner `1606` →
`1677`). The last scenario additionally verifies immediate `/agents`
and Ctrl-X detach without another restart, truthful completed status, original
TUI loss, repeated mid-turn owner reattachment and a successful attached follow-up.

[Final delivery evidence](background-search-delivery.md) contains the complete
matrix and [artifact manifest](background-search-artifacts.json) binds the results.
Supplemental all-tools Grok probes reached `web_search` but were blocked by actual
DuckDuckGo bot challenges; they are not counted as successful searches. Required
terminal-profile search passed on this same binary. No provider substitution.

## Historical first-feature-build evidence

The records below predate review corrections and are retained as historical
acceptance evidence, not substituted for the final-source rerun above.


Date: 2026-09-13. Host: user's Mac Studio, local native C11 release build.
This is first-feature-build evidence; the final delivery record must reconcile
it with post-review changes and rerun affected live scenarios where necessary.
No provider credentials or model reasoning traces are included here.

### Native Codex search and same-session follow-up — PASS

Used tny directly (not the Codex CLI), provider `codex`, model `gpt-6-astra`,
`ask --events=jsonl`, requiring an actual native search for the official Rust
Book ownership chapter. Canonical hosted tool-start/tool-end events were observed.
The saved assistant extras contained both `web_search_call` and `url_citation`.
A second `tny ask --resume <id> --json` request in the same session succeeded and
returned the previous source URL without requiring a fresh search:
`https://doc.rust-lang.org/book/ch04-00-understanding-ownership.html`.

Session: `802e9da8e7c09dac`. Both tny invocations exited 0. This independently
confirms native request acceptance, stream parsing, saved hosted items and
follow-up Responses input acceptance with the user's account.

An earlier POSIX documentation probe also executed hosted search successfully,
but the target returned403 and the provider supplied an empty annotations array.
That was not treated as proof of citation persistence; the Rust Book run above
supplies the required annotated response.

### Grok 4.6 DuckDuckGo search — PASS

Used tny directly with provider `grok`, model `grok-4.6`, terminal-only tools
(the user's runtime preference). The model invoked `tny web search` through the
terminal tool. Its canonical tool-end event contained real DuckDuckGo result
entries, and those results persisted into the session. The model answered with
the official Rust Book URL. A resumed follow-up turn returned that same URL.

Session: `ce279247a48a617f`. Search and follow-up both exited0. This is the
local DuckDuckGo search path, NOT a claim of Codex-hosted search on Grok.

The first all-tools-profile Grok probe was rejected before a search by the
provider's existing image_preview schema validation (top-level oneOf branches).
The required user-profile search passed using the terminal-first interface;
any separate all-tools compatibility change needs its own regression evidence.

### Grok 4.6 durable handoff and owner reattachment — PASS

Used a real PTY with the repository's terminal screen emulator, not a fake model.
Started provider `grok`, model `grok-4.6`, in terminal-only mode. Requested three
separate sequential tools: first writes a marker and sleeps, second writes a
marker and sleeps, third reads the markers. Pressed Left during the first tool.

Observed facts:

- The armed indicator appeared while FIRST_DONE was still absent. Handoff did
  not interrupt the tool and did not happen before its completed result.
- The original runner PID was `66164`; the successor PID was `66275`, preserving
  session `124f7297bb650a8f` and provider/model. This was an actual fresh process.
- The TUI displayed the Background agents dashboard, with the same session
  listed running. `tny agents --json` reported it before and after terminal loss.
- The original TUI process was killed after handoff; the successor continued.
- A new `tny agents` TUI selected and attached the session while the second tool
  was still running. The reattached status showed `grok`, `grok-4.6`, working.
- `/quit` from that active owner attachment exited0 without cancelling the
  worker. Another new dashboard attached the same ongoing turn again.
- The final result was TNY_BACKGROUND_COMPLETE. The marker file contained each
  line exactly once and in order: FIRST_STARTED, FIRST_DONE, SECOND_STARTED,
  SECOND_DONE. There was no repeated first-tool side effect.
- An additional user prompt sent through the attached TUI received
  TNY_REATTACH_FOLLOWUP, proving functional owner reattachment, not merely an
  observer displaying old output. Final quit exited0.

Test artifacts stayed in a private temporary HOME/workspace. Normal tny provider
login resolution was used; credential contents were never read by the driver,
copied into evidence, printed, or committed. Only these test sessions/processes
were stopped during cleanup. User settings were not overwritten.

The first lifecycle driver attempt used LF rather than the TUI's CR Enter key;
no turn started. The driver was corrected to use the same PTY input semantics as
the repository fixtures and to inspect the nested session.json result field.
The pass above comes from the complete corrected run, not that failed attempt.

### Reproduction outline

Compile and install a stable local build. From a scratch workspace, launch
`tny --provider grok --model grok-4.6`, request sequential marker-writing tools
with a pause in each, and press Left during the first pause. Open `tny agents`
in another terminal after handoff and press Enter on the running row. Verify
that quitting either dashboard or attached background view leaves the work
running, and that a subsequent attached prompt receives a new response.

For search, ask the same provider to run `tny web search` and cite a result.
For Codex, use `tny --provider codex --model gpt-6-astra ask --events=jsonl`
with no explicit search override and require native search; then resume that
session to test persisted hosted items. Exact test results must be recorded
from actual provider responses; availability is not a mocked guarantee.
