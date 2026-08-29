/* layout.h — the split-pane tree (docs/adr/0006).
 *
 * A window holds one binary tree: every leaf is a pane, every internal
 * node is a split with a direction and a ratio. Platform-free and
 * payload-agnostic — a leaf carries a `void *user` the caller owns (in
 * `tnytty gui` that is the pane's session + rasterizer), so geometry,
 * focus and close-and-reclaim are unit-tested with no window in sight.
 *
 * Geometry is device pixels, top-left origin, matching the framebuffer
 * the rasterizer paints into. Sibling rects never touch: a `divider`-wide
 * gap is left between them for the rule the rasterizer draws. */
#ifndef TNYTTY_UI_LAYOUT_H
#define TNYTTY_UI_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TT_SPLIT_LEAF = 0, /* no children: this node is a pane */
    TT_SPLIT_VERT,     /* children side by side (Cmd-D) */
    TT_SPLIT_HORZ,     /* children stacked (Cmd-Shift-D) */
} tt_split_dir;

/* Which way focus travels (Cmd-Opt-arrow). */
typedef enum {
    TT_MOVE_LEFT = 0,
    TT_MOVE_RIGHT,
    TT_MOVE_UP,
    TT_MOVE_DOWN,
} tt_move_dir;

typedef struct {
    int x, y, w, h;
} tt_rect;

typedef struct tt_node tt_node;
struct tt_node {
    tt_split_dir dir;
    tt_node *a, *b, *parent;
    /* Share of the parent's usable extent given to `a`, in thousandths;
     * 500 is the even split every new node starts at. */
    int ratio;
    tt_rect rect; /* filled by tt_layout_apply */
    void *user;   /* leaf payload, owned by the caller */
};

typedef struct {
    tt_node *root;
    tt_node *focus; /* always a leaf, never NULL while root exists */
    int divider;    /* device pixels of gap between siblings */
    tt_rect area;   /* the last area passed to tt_layout_apply */
} tt_layout;

/* One pane covering everything. Returns -1 on OOM (the layout is then
 * empty and every other call is a no-op). */
int tt_layout_init(tt_layout *l, int divider, void *user);
/* Frees every node. `drop` (may be NULL) is called with each leaf's
 * payload so the caller can tear its pane down. */
void tt_layout_free(tt_layout *l, void (*drop)(void *user, void *ctx), void *ctx);

/* Split `leaf` in `dir`, 50/50. The existing pane keeps the first half;
 * the new leaf (returned, and focused) takes the second. Returns NULL on
 * OOM or when `leaf` is not a leaf. Call tt_layout_apply afterwards. */
tt_node *tt_layout_split(tt_layout *l, tt_node *leaf, tt_split_dir dir, void *user);

/* Remove `leaf`; its sibling takes the parent's whole rect. Focus moves
 * to the nearest remaining leaf. Returns false when `leaf` was the last
 * pane — the caller closes the window — and the tree is left untouched.
 * The payload is not freed; the caller still owns it. */
bool tt_layout_close(tt_layout *l, tt_node *leaf);

/* Recompute every leaf's rect from `area`. */
void tt_layout_apply(tt_layout *l, tt_rect area);

/* The leaf whose rect contains (px, py), or NULL. A point in a divider
 * gap belongs to no pane. */
tt_node *tt_layout_at(const tt_layout *l, int px, int py);

/* The leaf that should take focus when moving `dir` out of `from`, or
 * NULL when there is none that way. Picks the neighbour whose rect
 * overlaps `from`'s centre line, so a pane split further does not send
 * focus to an unrelated corner. */
tt_node *tt_layout_neighbour(const tt_layout *l, const tt_node *from, tt_move_dir dir);

/* The next (`step` = 1) or previous (`step` = -1) leaf in left-to-right,
 * top-to-bottom leaf order, wrapping at both ends. Unlike tt_layout_neighbour this never
 * fails while a leaf exists: with one pane it returns that pane. */
tt_node *tt_layout_cycle(const tt_layout *l, const tt_node *from, int step);

/* Leaves in left-to-right, top-to-bottom tree order. Returns the number
 * of leaves, which may exceed `cap` (only `cap` are written). */
int tt_layout_leaves(const tt_layout *l, tt_node **out, int cap);
int tt_layout_count(const tt_layout *l);
bool tt_layout_is_leaf(const tt_node *n);

#endif
