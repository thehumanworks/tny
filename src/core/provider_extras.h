/* provider_extras.h — per-provider request add-ons (docs/adr/0067).
 *
 * Hosted OpenAI-compatible providers occasionally ask clients to carry one
 * small extra on every request (a session header, a client tag). Those
 * asks are not part of tny's provider model: they live here as a table of
 * self-contained entries matched at request time by base-URL host or
 * profile name, so one can be added, changed, or deleted without touching
 * the backend, the profile resolver, or any settings schema.
 *
 * Kill switch: TNY_PROVIDER_EXTRAS=0 disables every entry. */
#ifndef TNY_PROVIDER_EXTRAS_H
#define TNY_PROVIDER_EXTRAS_H

/* What an entry may look at. Borrowed pointers; any may be NULL. */
typedef struct {
    const char *provider_name; /* effective profile name ("opencodego", …) */
    const char *base_url;      /* resolved provider base URL */
    const char *session_id;    /* conversation id; NULL for non-turn requests */
} tny_request_scope;

/* Append the add-on header lines that apply to this request to out[0..cap).
 * Lines are malloc'd "Name: value" strings; free them with
 * tny_provider_extras_free. Returns the count written (0 when nothing
 * matches or extras are disabled). */
int tny_provider_extras_headers(const tny_request_scope *scope, char **out, int cap);
void tny_provider_extras_free(char **lines, int n);

/* Name of the first entry that matches the scope, or NULL — for
 * `tny doctor` / tests; never affects a request. */
const char *tny_provider_extras_match(const tny_request_scope *scope);

#endif
