/* skills.h — SKILL.md discovery and lazy load (docs/features/mcp-and-skills.md). */
#ifndef TNY_SKILLS_H
#define TNY_SKILLS_H

#include "core/config.h"

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

#endif
