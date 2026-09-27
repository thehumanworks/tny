/* acp_compat.h — private Claude tools-only compatibility policy (ADR 0177). */
#ifndef TNY_ACP_COMPAT_H
#define TNY_ACP_COMPAT_H

#include <stdbool.h>

#define ACP_CLAUDE_NAME        "@agentclientprotocol/claude-agent-acp"
#define ACP_CLAUDE_MIN_VERSION "0.75.1"

/* Exact identity, stable SemVer >= the tools-only minimum. Build metadata is
 * ignored; malformed, prerelease and overflowing components fail closed. */
bool acp_claude_tools_only(const char *name, const char *version);

#endif
