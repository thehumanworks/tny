#ifndef TNY_RETRIEVAL_SOURCES_H
#define TNY_RETRIEVAL_SOURCES_H
#include "core/session.h"
#define TNY_RETRIEVAL_SOURCE_LIMIT 32u
#define TNY_RETRIEVAL_BYTES_LIMIT  (64u * 1024u * 1024u)
/* Export only a caller-validated, current-session result handle. Internal
 * manifests are transported by the private child launch, never tool schemas. */
yyjson_mut_val *tny_retrieval_source_export(yyjson_mut_doc *doc, tny_session_state *owner,
                                            const char *artifact, const char *source,
                                            const char *coverage, size_t *bytes);
/* Import an internal manifest into ordinary child-local result artifacts.
 * Complete byte identity is verified before publishing any destination. */
yyjson_mut_val *tny_retrieval_source_import(yyjson_mut_doc *doc, tny_session_state *child,
                                            yyjson_val *entry, size_t *bytes);
/* Roll back only a handle created by the current import batch. No allocation. */
void tny_retrieval_source_discard(tny_session_state *child, const char *handle);
#endif
