#ifndef TNY_TOOLS_IMAGE_H
#define TNY_TOOLS_IMAGE_H
#include "core/tools.h"
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
/* Shared native/intercept adapter; success appends result JSON to out. It runs
 * exactly `prepared`, the plan the permission decision was made about; a NULL
 * plan refuses rather than resolving a new one. */
int tool_image_run(tools_env *, yyjson_val *, bool edit, tny_image_plan *prepared, buf_t *out,
                   char *err, size_t);
/* The role:"tool" message for one image call: the result JSON, or the usual
 * `error: ` marker followed by a safe object when there is one. */
char *tool_image_execute(tools_env *, yyjson_val *, bool edit, tny_image_plan *prepared);
#endif
