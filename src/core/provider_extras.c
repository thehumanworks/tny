/* provider_extras.c — per-provider request add-ons (docs/adr/0067).
 *
 * To add one: write a match() and a headers() function, append a row to
 * EXTRAS. To remove one: delete the row. Nothing else in the tree knows
 * the entry exists; the openai backend only calls
 * tny_provider_extras_headers once per POST. */
#include "core/provider_extras.h"
#include "net/net.h"
#include "util/util.h"

#include <strings.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    bool (*match)(const tny_request_scope *);
    /* append up to cap malloc'd header lines; return the count */
    int (*headers)(const tny_request_scope *, char **out, int cap);
} tny_provider_extra;

/* ---------- helpers shared by entries ---------- */

/* True when base_url's host is `domain` or a subdomain of it. */
static bool host_under(const char *base_url, const char *domain) {
    if (!base_url || !*base_url) return false;
    url_parts u;
    if (url_parse(base_url, &u) != 0) return false;
    size_t hl = strlen(u.host), dl = strlen(domain);
    if (hl < dl) return false;
    const char *tail = u.host + (hl - dl);
    if (strncasecmp(tail, domain, dl) != 0) return false;
    return hl == dl || tail[-1] == '.';
}

/* True when the profile name starts with `prefix`, ignoring case — the
 * shape NAME_BASE_URL / NAME_API_KEY env profiles produce ("opencodego"). */
static bool name_starts(const char *name, const char *prefix) {
    if (!name) return false;
    return strncasecmp(name, prefix, strlen(prefix)) == 0;
}

static char *header_line(const char *name, const char *value) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "%s: %s", name, value);
    if (buf_oom(&b)) {
        buf_free(&b);
        return NULL;
    }
    return b.data;
}

/* ---------- OpenCode Go (opencode.ai/zen/go) ----------
 * Their gateway keys prompt-cache routing and abuse detection on
 * `x-opencode-session`: one stable id per conversation — tny's session id.
 * Requests without it may be rejected (support notice, 2026-09). */

static bool opencode_match(const tny_request_scope *s) {
    return host_under(s->base_url, "opencode.ai") || name_starts(s->provider_name, "opencode");
}

static int opencode_headers(const tny_request_scope *s, char **out, int cap) {
    if (cap < 1 || !s->session_id || !*s->session_id) return 0;
    out[0] = header_line("x-opencode-session", s->session_id);
    return out[0] ? 1 : 0;
}

/* ---------- registry ---------- */

static const tny_provider_extra EXTRAS[] = {
    {"opencode-go", opencode_match, opencode_headers},
};

static bool extras_enabled(void) {
    const char *v = getenv("TNY_PROVIDER_EXTRAS");
    return !(v &&
             (strcmp(v, "0") == 0 || strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0));
}

const char *tny_provider_extras_match(const tny_request_scope *scope) {
    if (!scope || !extras_enabled()) return NULL;
    for (size_t i = 0; i < sizeof EXTRAS / sizeof EXTRAS[0]; i++)
        if (EXTRAS[i].match(scope)) return EXTRAS[i].name;
    return NULL;
}

int tny_provider_extras_headers(const tny_request_scope *scope, char **out, int cap) {
    if (!scope || !out || cap <= 0 || !extras_enabled()) return 0;
    int n = 0;
    for (size_t i = 0; i < sizeof EXTRAS / sizeof EXTRAS[0] && n < cap; i++) {
        if (!EXTRAS[i].match(scope)) continue;
        n += EXTRAS[i].headers(scope, out + n, cap - n);
    }
    return n;
}

void tny_provider_extras_free(char **lines, int n) {
    for (int i = 0; i < n; i++) free(lines[i]);
}
