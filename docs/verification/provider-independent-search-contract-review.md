# Provider-independent search contract review

Claude Code `--model fable --effort high`; reported `claude-fable-5-1`.
Successful contract amendment review (not implementation review), session
`beae4e11-603b-4144-8f9d-d2157a9de1a9`. The amended invariant table and ADR0109 record the
binding decisions. The service was drafted while this read-only amendment review
ran; all findings were reconciled before final verification. No claim is made
that this amendment review predates every draft write.

## Dispositions

All ten findings accepted: explicit builtin-inline exception; valid bearer AND
account id; bounded/pumped service token refresh with the existing atomic auth
write; exact wire fields and required tool choice; independent gpt-5.6-sol default
and web_search_model; named trusted endpoint overrides; local search under SSH;
Codex-labelled result provenance; enumerated docs/schema/index corrections;
function and terminal-intercept cancellation/background fixtures. Existing
numbered ADRs are unchanged; ADR0109 partially supersedes the old routing.

## Reviewer report

Review complete. Findings below are limited to concrete omissions or contradictions in `docs/verification/provider-independent-search.md` against the current code and docs.

**1. PS1 contradicts its own Boundaries for builtin Codex.** PS1 says all native-loop providers invoke the same search service. The Boundaries section keeps builtin ChatGPT-mode Codex on inline hosted search, where `tools.c:434` hides the local function entirely. Disposition: amend PS1 to "all native-loop providers except builtin ChatGPT-mode Codex, which keeps the ADR 0106 inline declaration", and state explicitly that `tny web search` under that provider now calls the search-only service. Today `docs/backends/codex.md:134` says the CLI runs the local DuckDuckGo path, which the new contract silently reverses.

**2. The login predicate is unnamed, and one credential state is unclassified.** `tny_codex_auth_present()` returns true for an API-key-only Codex CLI file and for a token without an account id. Image and speech gate on both an access token and an account id, CR/LF-free, via `tny_codex_credentials`. PS3 classes API-key-only as "no login", but says nothing about a token lacking an account id, which `tny doctor` already reports as a distinct state. Disposition: pin the predicate to the image_codex.c pattern and classify the missing account id as an error, not a DuckDuckGo route.

**3. Token refresh is a side effect the contract does not cover, and it conflicts with PS5.** The image and speech services call `tny_codex_refresh_if_stale()` before every request unless a flag token is set. That POSTs to auth.openai.com with a 30 second timeout that ignores cancellation, and rewrites the Codex CLI's auth file in place. PS5 requires cancellation to stay responsive during search. PS2 lists credential reads but no writes. Disposition: state that search refreshes like image and speech do, add the in-place refresh as the only permitted credential write under PS2, and require the refresh wait to be bounded or pumped so PS5 holds.

**4. PS4's "strict request schema" pins no fields.** The ChatGPT-mode request in openai.c sends stream true, store false, a required instructions string, the OpenAI-Beta responses header, the account-id header, and an originator header, to the responses path under the service base. The search-only request needs those pinned, plus: the exact hosted tool declaration, no local function tools, no include list, no previous_response_id, and a tool_choice value. Disposition: pin tool_choice to required so PS3's "no actual hosted search" case is deterministic rather than model-dependent.

**5. The independent model is not named.** PS2 requires an "independent model default/setting" but the contract never states the default or the settings key. The conversation model must never leak, since grok-4.6 would be rejected by the Codex service. Disposition: name the default, which should be the existing gpt-5.6-sol constant, and the settings key that overrides it.

**6. The trusted endpoint override is not named.** `tny_codex_service_base_url` honors a ctx field and the TNY_CODEX_BASE_URL environment variable. PS2 says "trusted Codex service endpoint" without naming either, and does not say that `web_search_url` never rebases the service. Disposition: name both accepted overrides and state that a URL override is a full switch to local search, not a rebase.

**7. Remote execution is unaddressed.** With `--ssh` active, a command override runs on the remote host through the terminal path. The contract does not say where the hosted service request runs. Credentials are local, so it must run on the local tny host. Disposition: add one sentence.

**8. Result provenance is unaddressed.** ADR 0106 requires the local path to identify DuckDuckGo honestly, and the parser emits a labelled header. A Grok user whose query now goes to OpenAI has no equivalent signal. Disposition: require the service result header to name the Codex search service, matching the existing DuckDuckGo header.

**9. Documentation contradictions to enumerate under PS6.** PS6 says docs must reflect the revised default but lists none. The following currently state the old default and will contradict the new behavior:

- ADR 0106, which says other native providers use DuckDuckGo by default. Its bytes must stay, so the new ADR must declare partial supersession and the ADR README index row for 0106 must be updated. The README is not an ADR.
- `docs/backends/codex.md` lines 131 to 134.
- `docs/features/mcp-and-skills.md` lines 33 and 146 to 147.
- `docs/cli.md` lines 41 and 906.
- `docs/settings.md` lines 75 to 76 and `docs/features/parity-with-fx.md` line 14.
- Header comment of `tools_web.c` and the comment at `tools.c:428`.
- The wasm behavior statement required by AGENTS.md for every new tool path. On wasm only the flag and environment credential sources exist.

**10. PS5 evidence should include the terminal-intercept path.** The original contract records that the Mac Studio exec inherits TNY_TOOLS set to terminal, which hides the function tool. Under that profile the model reaches search only through the intercepted `tny web search` call. Disposition: require the slow-search cancellation and search-plus-background fixtures to cover both the function tool and the intercept.

No contradictions were found with existing background behavior. Service results are ordinary function results, so the BG1 checkpoint rule applies unchanged, and the hosted-boundary special case at `openai.c:2982` stays limited to builtin Codex.
