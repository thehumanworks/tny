# ADR 0136: Preserve optional tool arguments on Responses

Date: 2026-09-18. Status: accepted.

## Context and reproduction

The reported repeated `SUBAGENT_INVALID_ARGUMENT: create allocates the child
id; omit id` still occurs at `89bcd5918da0e225e1806813a206d007daafac0a`.
A direct tool probe with `action=create`, `id=""` and a nonempty prompt
returned that exact diagnostic. The model-facing schema in that session
required `action`, `id` and `prompt`, although the source schema requires
only `action` and documents action-dependent arguments.

`tny_openai_responses_tools` flattened the Chat Completions schema without
specifying `strict`. The Responses service can normalize such a schema into
strict mode, requiring every property. Since `id` is a string rather than a
nullable type, a schema-conforming model cannot omit it or use null. Steer
messages telling the model to omit it cannot resolve the contradiction.
`inspect` and `lifecycle` have the corresponding forbidden-prompt problem.

The primary function-calling guide, checked on 2026-09-18, documents the
Responses normalization default, Chat Completions' non-strict default, and
explicit `strict: false` as the opt-out:
`https://developers.openai.com/api/docs/guides/function-calling#strict-mode`.

The regression fixture models schema-conforming calls after normalization,
not a claim that the server inserts argument values. Against the unchanged
release binary, two Responses creates returned the exact reported error;
the identical Chat Completions workflow completed two children and eight
tool calls. Two new wire-translation unit assertions also failed before
the production change. Existing mocks had emitted hand-authored valid
arguments without modeling the service's default normalization.

## Decisions

1. At the existing Responses translation boundary, emit `strict: false`
   when a function has no strict setting or a null/default setting. Preserve
   explicit `true` and `false`. Leave parameter schemas, required lists,
   nullable types, descriptions and other metadata unchanged. This makes
   translation preserve Chat Completions semantics for builtins, MCP tools
   and registered custom tools rather than special-casing one tool.
2. Keep subagent validation unchanged. Even empty, null, named or existing
   IDs on `create` remain invalid. Do not silently discard fields, invent
   IDs, resume a session, split the public tool into multiple tools, or
   widen permissions. Runtime action validation remains authoritative.
3. Do not enable strict mode globally by making every optional argument
   nullable. That changes omission/default semantics across unrelated tools
   and needs a separate design. Explicit strict schemas continue to opt in.
4. Strengthen the shared Responses mock to require an explicit boolean
   strict setting. Exercise two independent durable children through
   create/message/inspect/lifecycle on both wires, in addition to the
   existing Codex-profile, permission, process and diagnostic fixtures.
   These extend existing files and runners; no new make target, fixture
   directory, dependency, scheduler or Nix input is needed.
5. The local GCC 16.2.1 debug build exposed a pre-existing unused-but-set
   warning for the volatile work counter in `parallel_probe_item`. Read it
   with `(void)spin` so the required tests build without suppressing warnings
   or changing their scheduling workload. This is test-only portability work.
6. Run real Codex parent/child sessions from a scratch workspace with bounded
   steps. Preserve user settings and publish only sanitized tool/session
   evidence, never credentials. Commit directly to main as requested, then
   use the existing green-ci/nix/sdk auto-release gate. Do not bypass gates
   or publish an unverified tag to satisfy the release request.

## Compatibility and tradeoffs

The native provider loop, public C ABI, tool names and stored sessions are
unchanged. Explicit non-strict mode gives up service-level schema enforcement
for tools that never requested it; local validation and permission checks
still apply. This also repairs unrelated optional/default argument semantics
on Responses. Chat Completions requests are unchanged. Structured final
answer schemas (`text.format`) retain their separate strict setting.

The same translation runs on wasm. No process support is added: native
subagents remain unsupported under SSH and embedded contexts, while wasm
create/message still return a clean unsupported-context error and stored
inspect/lifecycle retain their existing behavior.

## Verification

The reproducible commands and sanitized live multi-agent evidence are recorded
in `docs/verification/subagent-tool-schema.md`. Acceptance requires the focused
red/green regression, full `make test`, `make quality`, `make leaks`, strict
compiler checks, a release-size check, and a successful live run through the
actual `subagent` tool, not shell-created stand-ins. CI's existing platform,
SDK and Nix gates decide release eligibility.
