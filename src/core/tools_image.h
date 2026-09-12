#ifndef TNY_TOOLS_IMAGE_H
#define TNY_TOOLS_IMAGE_H
#include "core/tools.h"
#include "core/image_export.h"
#include "core/image_service.h"
/* Output, an optional replay/artifact record and up to five references. */
#define TNY_IMAGE_PATHS_MAX (TNY_IMAGE_REFERENCES_MAX + 2)
/* Canonical operation scope for grants: provider, output, any record the call
 * reruns, and every reference path that would actually be uploaded — resolved
 * from records before the grant, never a raw argument string. NULL on invalid
 * arguments; no referenced image is read and nothing authenticates.
 * On success the resolved plan is handed to `prepared` (ADR 0095): the caller
 * owns it until the call finishes and releases it with tool_image_plan_free.
 * `prepared` may be NULL when only the description is wanted. */
char *tool_image_detail(tools_env *, yyjson_val *, bool edit, tny_image_plan **prepared,
                        char **error);
void tool_image_plan_free(tny_image_plan *);
char *tool_image_preview_detail(tools_env *, yyjson_val *, tny_image_preview_selection **, char **);
char *tool_image_preview_execute(tools_env *, const tny_image_preview_selection *);
/* Shared native/intercept adapter; success appends result JSON to out. It runs
 * exactly `prepared`, the plan the permission decision was made about; a NULL
 * plan refuses rather than resolving a new one. */
int tool_image_run(tools_env *, yyjson_val *, bool edit, tny_image_plan *prepared, buf_t *out,
                   char *err, size_t);
/* The role:"tool" message for one image call: the result JSON, or the usual
 * `error: ` marker followed by a safe object when there is one. */
char *tool_image_execute(tools_env *, yyjson_val *, bool edit, tny_image_plan *prepared);

/* Output plus up to 64 ordered contact-sheet sources. */
#define TNY_IMAGE_EXPORT_PATHS_MAX (TNY_IMAGE_EXPORT_SOURCES_MAX + 1)
/* The sensitive grant identity of one local transform (ADR 0094): operation,
 * ordered canonical sources with the hash of their exact current bytes, the
 * canonical destination and every setting that changes the written bytes.
 * NULL on invalid arguments; nothing is converted and nothing is written. */
char *tool_image_export_detail(tools_env *, yyjson_val *, bool sheet, char **error);
/* Shared native/intercept adapter. The identity above is rebuilt and
 * rechecked here, so a source, record or destination that changed after the
 * grant needs a new one. Success appends result JSON to out; any failure
 * after commit appends the retained object and still fails. */
/* approved_detail is borrowed from the already-authorized prepared call.
 * NULL requires a matching permanent grant (or yolo). It is never persisted. */
int tool_image_export_run(tools_env *, yyjson_val *, bool sheet, buf_t *out, char *err, size_t,
                          const char *approved_detail);
char *tool_image_export_execute(tools_env *, yyjson_val *, bool sheet, const char *approved_detail);
#endif
