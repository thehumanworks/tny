/* workspace.h — durable GUI topology, independent of the PTY owner.
 *
 * The broker owns live sessions.  This document owns only presentation:
 * ordered tabs, split trees, focus, and stable session IDs. */
#ifndef TNYTTY_UI_WORKSPACE_H
#define TNYTTY_UI_WORKSPACE_H

#include "ui/layout.h"

#include <stddef.h>

#define TT_WORKSPACE_MAX_TABS  16
#define TT_WORKSPACE_MAX_PANES 32
#define TT_WORKSPACE_MAX_NODES (TT_WORKSPACE_MAX_PANES * 2 - 1)

typedef struct {
    tt_split_dir dir;
    int ratio;
    int a, b; /* child indexes, -1 for a leaf */
    char session_id[9];
} tt_workspace_node;

typedef struct {
    int root;
    int focus;
    int node_count;
    tt_workspace_node nodes[TT_WORKSPACE_MAX_NODES];
} tt_workspace_tab;

typedef struct {
    int active;
    int tab_count;
    tt_workspace_tab tabs[TT_WORKSPACE_MAX_TABS];
} tt_workspace;

void tt_workspace_init(tt_workspace *ws);

/* Resolve $TNYTTY_STATE_DIR/workspace, then $XDG_STATE_HOME/tnytty/workspace,
 * then ~/.local/state/tnytty/workspace.  This does not create directories. */
int tt_workspace_path(char *out, size_t cap);

/* Binary, versioned, bounded, mode-0600 state.  Save creates the state
 * directory mode 0700 and replaces the document atomically.  Load returns
 * 1 when no document exists, 0 on success, and -1 for malformed/I/O errors. */
int tt_workspace_save(const char *path, const tt_workspace *ws, char *err, size_t errcap);
int tt_workspace_load(const char *path, tt_workspace *ws, char *err, size_t errcap);

/* Structural validation is public so GUI conversion can reject an invalid
 * in-memory tree before replacing the last good document. */
int tt_workspace_validate(const tt_workspace *ws, char *err, size_t errcap);

#endif
