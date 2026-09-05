#ifndef TNY_TOOLS_IMAGE_H
#define TNY_TOOLS_IMAGE_H
#include "core/tools.h"
/* Canonical operation scope for grants: provider, output and all uploaded
 * reference paths. NULL on invalid arguments; no file-content reads or authentication. */
char *tool_image_detail(tools_env *, yyjson_val *, bool edit, char **error);
/* Shared native/intercept adapter; success appends result JSON to out. */
int tool_image_run(tools_env *, yyjson_val *, bool edit, buf_t *out, char *err, size_t);
#endif
