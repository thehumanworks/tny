#include "backends/acp/acp_compat.h"

#include <stdint.h>
#include <string.h>

static bool digit(char c) { return c >= '0' && c <= '9'; }

static bool component(const char **cursor, uint32_t *value) {
    const char *p = *cursor;
    if (!digit(*p) || (*p == '0' && digit(p[1]))) return false;
    uint32_t n = 0;
    do {
        uint32_t d = (uint32_t)(*p - '0');
        if (n > (UINT32_MAX - d) / 10) return false;
        n = n * 10 + d;
        p++;
    } while (digit(*p));
    *cursor = p;
    *value = n;
    return true;
}

static bool build_metadata(const char *p) {
    if (!*p) return true;
    if (*p++ != '+') return false; /* Prereleases are not verified stable releases. */
    bool nonempty = false;
    for (; *p; p++) {
        if (*p == '.') {
            if (!nonempty) return false;
            nonempty = false;
        } else if (digit(*p) || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-') {
            nonempty = true;
        } else {
            return false;
        }
    }
    return nonempty;
}

bool acp_claude_tools_only(const char *name, const char *version) {
    if (!name || strcmp(name, ACP_CLAUDE_NAME) != 0 || !version) return false;
    const char *p = version;
    uint32_t major, minor, patch;
    if (!component(&p, &major) || *p != '.') return false;
    p++;
    if (!component(&p, &minor) || *p != '.') return false;
    p++;
    if (!component(&p, &patch) || !build_metadata(p)) return false;
    /* Numeric, not lexical, order. Keep the documented floor and Lean model in
     * sync; tests replay the proof project's exported compatibility table. */
    return major > 0 || minor > 75 || (minor == 75 && patch >= 1);
}
