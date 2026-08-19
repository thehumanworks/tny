/* openai.h — native backend extras: the frontend binds the session,
 * permission engine, and approval hook before send(). */
#ifndef TNY_OPENAI_H
#define TNY_OPENAI_H

#include "core/backend.h"
#include "core/session.h"
#include "core/perm.h"

void tny_backend_openai_bind(tny_backend *b, tny_session *session,
                             perm_engine *perm,
                             tny_perm_decision (*prompt)(const char *tool,
                                                         const char *summary,
                                                         void *ud),
                             void *prompt_ud);

/* Number of agent steps taken in the last turn + tool call log (JSON array
 * text, borrowed until next send). */
int tny_backend_openai_steps(tny_backend *b);
const char *tny_backend_openai_toolcalls_json(tny_backend *b);

#endif
