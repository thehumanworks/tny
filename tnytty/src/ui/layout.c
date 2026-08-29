#include "ui/layout.h"

#include <stdlib.h>
#include <string.h>

#define RATIO_ONE 1000

bool tt_layout_is_leaf(const tt_node *n) { return n && n->dir == TT_SPLIT_LEAF; }

static tt_node *node_new(tt_node *parent, void *user) {
    tt_node *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->dir = TT_SPLIT_LEAF;
    n->parent = parent;
    n->ratio = RATIO_ONE / 2;
    n->user = user;
    return n;
}

int tt_layout_init(tt_layout *l, int divider, void *user) {
    memset(l, 0, sizeof *l);
    l->divider = divider < 0 ? 0 : divider;
    l->root = node_new(NULL, user);
    if (!l->root) return -1;
    l->focus = l->root;
    return 0;
}

static void free_tree(tt_node *n, void (*drop)(void *, void *), void *ctx) {
    if (!n) return;
    free_tree(n->a, drop, ctx);
    free_tree(n->b, drop, ctx);
    if (n->dir == TT_SPLIT_LEAF && drop) drop(n->user, ctx);
    free(n);
}

void tt_layout_free(tt_layout *l, void (*drop)(void *, void *), void *ctx) {
    free_tree(l->root, drop, ctx);
    l->root = l->focus = NULL;
}

tt_node *tt_layout_split(tt_layout *l, tt_node *leaf, tt_split_dir dir, void *user) {
    if (!l->root || !tt_layout_is_leaf(leaf) || dir == TT_SPLIT_LEAF) return NULL;
    tt_node *a = node_new(leaf, leaf->user);
    tt_node *b = node_new(leaf, user);
    if (!a || !b) {
        free(a);
        free(b);
        return NULL;
    }
    /* The split happens in place: `leaf` becomes the internal node, so
     * every pointer the caller already holds to it stays valid. */
    a->rect = leaf->rect;
    b->rect = leaf->rect;
    leaf->dir = dir;
    leaf->ratio = RATIO_ONE / 2;
    leaf->a = a;
    leaf->b = b;
    leaf->user = NULL;
    l->focus = b; /* iTerm2 convention: the new pane takes focus */
    tt_layout_apply(l, l->area);
    return b;
}

/* Any leaf under n, preferring the first in reading order. */
static tt_node *first_leaf(tt_node *n) {
    while (n && !tt_layout_is_leaf(n)) n = n->a;
    return n;
}

bool tt_layout_close(tt_layout *l, tt_node *leaf) {
    if (!l->root || !tt_layout_is_leaf(leaf)) return false;
    tt_node *parent = leaf->parent;
    if (!parent) return false; /* the last pane: the window closes instead */
    tt_node *sibling = parent->a == leaf ? parent->b : parent->a;
    /* Whether focus has to move is decided before either node is freed. */
    bool refocus = l->focus == leaf || l->focus == sibling;

    /* The sibling is spliced into the parent's slot in place, so the
     * parent pointer the tree already holds keeps addressing the same
     * subtree and the sibling reclaims the whole rect. */
    parent->dir = sibling->dir;
    parent->ratio = sibling->ratio;
    parent->user = sibling->user;
    parent->a = sibling->a;
    parent->b = sibling->b;
    if (parent->a) parent->a->parent = parent;
    if (parent->b) parent->b->parent = parent;
    free(sibling);
    free(leaf);

    if (refocus) l->focus = first_leaf(parent);
    tt_layout_apply(l, l->area);
    return true;
}

static void apply_node(tt_node *n, tt_rect r, int div) {
    n->rect = r;
    if (n->dir == TT_SPLIT_LEAF) return;
    tt_rect a = r, b = r;
    if (n->dir == TT_SPLIT_VERT) {
        int usable = r.w - div;
        if (usable < 0) usable = 0;
        int aw = usable * n->ratio / RATIO_ONE;
        a.w = aw;
        b.x = r.x + aw + div;
        b.w = r.w - aw - div;
        if (b.w < 0) b.w = 0;
    } else {
        int usable = r.h - div;
        if (usable < 0) usable = 0;
        int ah = usable * n->ratio / RATIO_ONE;
        a.h = ah;
        b.y = r.y + ah + div;
        b.h = r.h - ah - div;
        if (b.h < 0) b.h = 0;
    }
    apply_node(n->a, a, div);
    apply_node(n->b, b, div);
}

void tt_layout_apply(tt_layout *l, tt_rect area) {
    l->area = area;
    if (l->root) apply_node(l->root, area, l->divider);
}

static bool in_rect(const tt_rect *r, int x, int y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

tt_node *tt_layout_at(const tt_layout *l, int px, int py) {
    tt_node *n = l->root;
    while (n && !tt_layout_is_leaf(n)) {
        if (in_rect(&n->a->rect, px, py)) n = n->a;
        else if (in_rect(&n->b->rect, px, py)) n = n->b;
        else return NULL; /* the divider gap belongs to neither child */
    }
    return n && in_rect(&n->rect, px, py) ? n : NULL;
}

/* Focus navigation is a hit test just past the edge being crossed, on
 * the centre line of the pane being left: it needs no tree walk and
 * gives the same answer for nested splits. The centre can land exactly
 * on a divider of the neighbouring column, so the near and far edges are
 * tried after it rather than reporting "nothing that way". */
tt_node *tt_layout_neighbour(const tt_layout *l, const tt_node *from, tt_move_dir dir) {
    if (!from || !l->root) return NULL;
    const tt_rect *r = &from->rect;
    int gap = l->divider;
    bool horizontal = dir == TT_MOVE_LEFT || dir == TT_MOVE_RIGHT;
    int fixed = dir == TT_MOVE_LEFT    ? r->x - gap - 1
                : dir == TT_MOVE_RIGHT ? r->x + r->w + gap
                : dir == TT_MOVE_UP    ? r->y - gap - 1
                                       : r->y + r->h + gap;
    /* Along the shared edge: middle first, then the two ends. */
    int lo = horizontal ? r->y : r->x;
    int len = horizontal ? r->h : r->w;
    int probe[3] = {lo + len / 2, lo, lo + len - 1};
    for (int i = 0; i < 3; i++) {
        tt_node *hit =
            horizontal ? tt_layout_at(l, fixed, probe[i]) : tt_layout_at(l, probe[i], fixed);
        if (hit && hit != from) return hit;
    }
    return NULL;
}

static int collect(tt_node *n, tt_node **out, int cap, int used) {
    if (!n) return used;
    if (tt_layout_is_leaf(n)) {
        if (used < cap && out) out[used] = n;
        return used + 1;
    }
    used = collect(n->a, out, cap, used);
    return collect(n->b, out, cap, used);
}

int tt_layout_leaves(const tt_layout *l, tt_node **out, int cap) {
    return collect(l->root, out, cap, 0);
}

int tt_layout_count(const tt_layout *l) { return collect(l->root, NULL, 0, 0); }

#define CYCLE_MAX 256

tt_node *tt_layout_cycle(const tt_layout *l, const tt_node *from, int step) {
    tt_node *leaves[CYCLE_MAX];
    int n = collect(l->root, leaves, CYCLE_MAX, 0);
    if (n > CYCLE_MAX) n = CYCLE_MAX;
    if (n < 1) return NULL;
    int at = 0;
    for (int i = 0; i < n; i++)
        if (leaves[i] == from) {
            at = i;
            break;
        }
    int to = (at + (step < 0 ? -1 : 1) + n) % n;
    return leaves[to];
}
