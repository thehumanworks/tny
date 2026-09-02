/* skills.h — SKILL.md discovery and lazy load (docs/features/mcp-and-skills.md). */
#ifndef TNY_SKILLS_H
#define TNY_SKILLS_H

#include "core/config.h"
#include "core/session.h"

typedef struct {
    char *name;
    char *description;
    char *dir; /* directory containing SKILL.md */
} skill_meta;

/* Discover skill metadata (frontmatter only, bodies are not read).
 * Search order per docs: workspace upward (stop before $HOME) in skills/,
 * .agents/skills/, .claude/skills/, .codex/skills/, .cursor/skills/,
 * .opencode/skills/; then ~/.tny/skills/ and the hidden names under $HOME. */
skill_meta *skills_discover(tny_ctx *ctx, int *count);
void skills_free(skill_meta *s, int count);

/* Load a skill body by name; malloc'd or NULL. */
char *skills_load(tny_ctx *ctx, const char *name);

/* Copy a skill directory into ~/.tny/skills/<name>. 0 on success. */
int skills_install(tny_ctx *ctx, const char *src_dir, char *err, size_t errlen);

/* ---- mention injection (docs/adr/0056) ----
 * A prompt mentions a skill when it contains `/<name>` or `$<name>` as a
 * whole token: at the start of the text or after whitespace, and followed by
 * the end, whitespace, or punctuation other than `/`, `-`, `_` (so `/foo`
 * does not match `foobar`, `foo-bar`, or the path `/foo/bar`). Returns the
 * byte offset of the first mention or SIZE_MAX. */
size_t skills_mention_pos(const char *text, const char *name);

/* Indices into `skills` of every mentioned skill, in order of first
 * appearance, each at most once. Returns the count written to `out`. */
int skills_mentioned(const char *text, const skill_meta *skills, int n, int *out, int max);

/* Build the turn text for `prompt`: every mentioned skill body wrapped in a
 * `<skill name=... path=...>` block ahead of the user's text, or NULL when
 * nothing is mentioned (send the prompt unchanged). Bodies larger than
 * ctx->max_tool_result_bytes are cut like a tool result; `native` adds the
 * read_tool_result handle the native loop can follow. A skill already
 * recorded in the session's verbatim window is not re-sent: a one-line
 * reminder replaces the body. `names`/`n_names` receive the injected skill
 * names (malloc'd array of malloc'd strings, reminders included) so the
 * caller can record them after the send succeeds. */
char *skills_inject(tny_ctx *ctx, tny_session_state *session, const char *prompt, bool native,
                    char ***names, int *n_names);
void skills_names_free(char **names, int n);

#endif
