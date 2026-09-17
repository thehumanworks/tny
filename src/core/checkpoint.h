/* Private restart snapshots. Context snapshots include credentials and MUST
 * travel only through anonymous IPC, never files, argv or diagnostics. */
#ifndef TNY_CHECKPOINT_H
#define TNY_CHECKPOINT_H
#ifdef __cplusplus
extern "C" {
#endif
#include "core/config.h"

yyjson_mut_val *tny_checkpoint_context(yyjson_mut_doc *doc, const tny_ctx *ctx);
tny_ctx *tny_checkpoint_context_restore(yyjson_val *root);
yyjson_mut_val *tny_checkpoint_public(yyjson_mut_doc *d, const tny_ctx *ctx);
tny_ctx *tny_checkpoint_recover(tny_ctx *resolved, yyjson_val *public_context);
#ifdef __cplusplus
}
#endif
#endif
