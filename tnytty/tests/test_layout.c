/* test_layout.c — the split-pane tree (docs/adr/0006): splitting,
 * closing, rectangle arithmetic, hit testing and focus movement. The
 * tree is payload-agnostic, so the "panes" here are just tagged
 * pointers: no session, no window, no pixels but the ones it computes. */
#include "greatest.h"
#include "ui/layout.h"
#include "ui/render.h"

#include <string.h>

/* Leaf payloads: distinguishable addresses, nothing more. */
static char pane_a, pane_b, pane_c, pane_d;

static tt_rect area_of(int w, int h) {
    tt_rect r = {0, 0, w, h};
    return r;
}

TEST one_pane_fills_the_area(void) {
    tt_layout l;
    ASSERT_EQ(0, tt_layout_init(&l, 1, &pane_a));
    tt_layout_apply(&l, area_of(800, 600));
    ASSERT_EQ(1, tt_layout_count(&l));
    ASSERT(tt_layout_is_leaf(l.root));
    ASSERT_EQ(l.root, l.focus);
    ASSERT_EQ(0, l.root->rect.x);
    ASSERT_EQ(0, l.root->rect.y);
    ASSERT_EQ(800, l.root->rect.w);
    ASSERT_EQ(600, l.root->rect.h);
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

TEST vertical_split_puts_the_new_pane_on_the_right(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(801, 600));
    tt_node *root = l.root;
    tt_node *b = tt_layout_split(&l, root, TT_SPLIT_VERT, &pane_b);
    ASSERT(b != NULL);
    ASSERT_EQ(2, tt_layout_count(&l));
    ASSERT_EQ(b, l.focus); /* iTerm2: the new pane takes focus */

    tt_node *a = root->a;
    ASSERT_EQ(&pane_a, a->user);
    ASSERT_EQ(&pane_b, b->user);
    /* 801 px minus a 1 px rule leaves 800 to halve; the odd pixel goes
     * to the right-hand pane, and the two never overlap the divider. */
    ASSERT_EQ(0, a->rect.x);
    ASSERT_EQ(400, a->rect.w);
    ASSERT_EQ(401, b->rect.x);
    ASSERT_EQ(400, b->rect.w);
    ASSERT_EQ(600, a->rect.h);
    ASSERT_EQ(600, b->rect.h);
    ASSERT_EQ(a->rect.y, b->rect.y);
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

TEST horizontal_split_puts_the_new_pane_below(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(800, 601));
    tt_node *root = l.root;
    tt_node *b = tt_layout_split(&l, root, TT_SPLIT_HORZ, &pane_b);
    tt_node *a = root->a;
    ASSERT_EQ(0, a->rect.y);
    ASSERT_EQ(300, a->rect.h);
    ASSERT_EQ(301, b->rect.y);
    ASSERT_EQ(300, b->rect.h);
    ASSERT_EQ(800, a->rect.w);
    ASSERT_EQ(800, b->rect.w);
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

/* Odd sizes must not lose or double-count a pixel: the two halves plus
 * the rule always add back up to the parent. */
TEST odd_sizes_never_lose_or_overlap_a_pixel(void) {
    for (int w = 3; w < 60; w++) {
        tt_layout l;
        tt_layout_init(&l, 1, &pane_a);
        tt_layout_apply(&l, area_of(w, w + 1));
        tt_node *root = l.root;
        tt_layout_split(&l, root, TT_SPLIT_VERT, &pane_b);
        ASSERT_EQ(w, root->a->rect.w + 1 + root->b->rect.w);
        ASSERT_EQ(root->a->rect.x + root->a->rect.w + 1, root->b->rect.x);
        tt_layout_free(&l, NULL, NULL);
    }
    PASS();
}

TEST nested_splits_subdivide_only_their_own_rect(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(401, 301));
    tt_node *root = l.root;
    tt_node *right = tt_layout_split(&l, root, TT_SPLIT_VERT, &pane_b);
    tt_node *bottom_right = tt_layout_split(&l, right, TT_SPLIT_HORZ, &pane_c);
    ASSERT_EQ(3, tt_layout_count(&l));

    tt_node *left = root->a;
    ASSERT_EQ(200, left->rect.w);
    ASSERT_EQ(301, left->rect.h); /* the left pane never learned about it */
    tt_node *top_right = right->a;
    ASSERT_EQ(201, top_right->rect.x);
    ASSERT_EQ(150, top_right->rect.h);
    ASSERT_EQ(201, bottom_right->rect.x);
    ASSERT_EQ(151, bottom_right->rect.y);
    ASSERT_EQ(150, bottom_right->rect.h);
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

TEST closing_a_pane_gives_its_space_to_the_sibling(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(801, 600));
    tt_node *root = l.root;
    tt_node *b = tt_layout_split(&l, root, TT_SPLIT_VERT, &pane_b);
    ASSERT(tt_layout_close(&l, b));
    ASSERT_EQ(1, tt_layout_count(&l));
    ASSERT(tt_layout_is_leaf(l.root));
    ASSERT_EQ(&pane_a, l.root->user);
    ASSERT_EQ(l.root, l.focus); /* focus followed the surviving pane */
    ASSERT_EQ(801, l.root->rect.w);
    ASSERT_EQ(600, l.root->rect.h);
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

TEST closing_the_last_pane_is_refused(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(800, 600));
    /* The caller closes the window instead; the tree stays intact. */
    ASSERT_FALSE(tt_layout_close(&l, l.root));
    ASSERT_EQ(1, tt_layout_count(&l));
    ASSERT_EQ(&pane_a, l.root->user);
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

TEST closing_a_nested_pane_promotes_its_subtree(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(401, 301));
    tt_node *root = l.root;
    tt_node *right = tt_layout_split(&l, root, TT_SPLIT_VERT, &pane_b);
    tt_layout_split(&l, right, TT_SPLIT_HORZ, &pane_c);
    /* Closing the left pane hands the whole area to the right-hand
     * split, which keeps its own two panes stacked. */
    ASSERT(tt_layout_close(&l, root->a));
    ASSERT_EQ(2, tt_layout_count(&l));
    ASSERT_EQ(TT_SPLIT_HORZ, l.root->dir);
    ASSERT_EQ(0, l.root->a->rect.x);
    ASSERT_EQ(401, l.root->a->rect.w);
    ASSERT_EQ(401, l.root->b->rect.w);
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

TEST hit_testing_finds_the_pane_and_skips_the_divider(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(801, 600));
    tt_node *root = l.root;
    tt_node *b = tt_layout_split(&l, root, TT_SPLIT_VERT, &pane_b);
    tt_node *a = root->a;
    ASSERT_EQ(a, tt_layout_at(&l, 0, 0));
    ASSERT_EQ(a, tt_layout_at(&l, 399, 599));
    ASSERT_EQ(b, tt_layout_at(&l, 401, 0));
    ASSERT_EQ(b, tt_layout_at(&l, 800, 599));
    ASSERT_EQ(NULL, tt_layout_at(&l, 400, 300)); /* the rule itself */
    ASSERT_EQ(NULL, tt_layout_at(&l, 801, 0));   /* outside the area */
    ASSERT_EQ(NULL, tt_layout_at(&l, 10, 600));
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

TEST focus_moves_to_the_neighbour_in_that_direction(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(401, 301));
    tt_node *root = l.root;
    tt_node *right = tt_layout_split(&l, root, TT_SPLIT_VERT, &pane_b);
    tt_node *bottom_right = tt_layout_split(&l, right, TT_SPLIT_HORZ, &pane_c);
    tt_node *left = root->a;
    tt_node *top_right = right->a;

    ASSERT_EQ(left, tt_layout_neighbour(&l, top_right, TT_MOVE_LEFT));
    ASSERT_EQ(bottom_right, tt_layout_neighbour(&l, top_right, TT_MOVE_DOWN));
    ASSERT_EQ(top_right, tt_layout_neighbour(&l, bottom_right, TT_MOVE_UP));
    /* Nothing that way: the edge of the window, not a wrap-around. */
    ASSERT_EQ(NULL, tt_layout_neighbour(&l, left, TT_MOVE_LEFT));
    ASSERT_EQ(NULL, tt_layout_neighbour(&l, left, TT_MOVE_UP));
    ASSERT_EQ(NULL, tt_layout_neighbour(&l, top_right, TT_MOVE_RIGHT));
    /* The left pane spans both right-hand rows; moving right lands on
     * the one its centre line points at, not on an arbitrary corner. */
    ASSERT_EQ(top_right, tt_layout_neighbour(&l, left, TT_MOVE_RIGHT));
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

TEST cycling_walks_every_pane_in_leaf_order_and_wraps(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(401, 301));
    tt_node *root = l.root;
    tt_node *right = tt_layout_split(&l, root, TT_SPLIT_VERT, &pane_b);
    tt_node *bottom_right = tt_layout_split(&l, right, TT_SPLIT_HORZ, &pane_c);
    tt_node *left = root->a;
    tt_node *top_right = right->a;

    /* Left, then the right-hand column top to bottom. */
    ASSERT_EQ(top_right, tt_layout_cycle(&l, left, 1));
    ASSERT_EQ(bottom_right, tt_layout_cycle(&l, top_right, 1));
    ASSERT_EQ(left, tt_layout_cycle(&l, bottom_right, 1)); /* wraps forward */
    ASSERT_EQ(bottom_right, tt_layout_cycle(&l, left, -1));
    ASSERT_EQ(top_right, tt_layout_cycle(&l, bottom_right, -1));

    /* Cycling always lands somewhere, even where no direction has a
     * neighbour: the bottom-right pane has nothing to its right. */
    ASSERT_EQ(NULL, tt_layout_neighbour(&l, bottom_right, TT_MOVE_RIGHT));
    ASSERT(tt_layout_cycle(&l, bottom_right, 1) != NULL);
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

TEST cycling_a_single_pane_stays_put(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(400, 300));
    ASSERT_EQ(l.root, tt_layout_cycle(&l, l.root, 1));
    ASSERT_EQ(l.root, tt_layout_cycle(&l, l.root, -1));
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

TEST cycling_after_a_close_skips_the_pane_that_left(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(801, 601));
    tt_node *root = l.root;
    tt_node *second = tt_layout_split(&l, root, TT_SPLIT_VERT, &pane_b);
    tt_node *third = tt_layout_split(&l, second, TT_SPLIT_VERT, &pane_c);
    tt_node *first = root->a;
    ASSERT_EQ(3, tt_layout_count(&l));

    ASSERT(tt_layout_close(&l, third));
    ASSERT_EQ(2, tt_layout_count(&l));
    tt_node *left = l.root->a, *right = l.root->b;
    ASSERT_EQ(first, left);
    ASSERT_EQ(&pane_b, right->user);
    ASSERT_EQ(right, tt_layout_cycle(&l, left, 1));
    ASSERT_EQ(left, tt_layout_cycle(&l, right, 1));
    ASSERT_EQ(right, tt_layout_cycle(&l, left, -1));
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

/* The whole point of the rects: they become a session's cols and rows. */
TEST pane_rects_become_cols_and_rows(void) {
    tt_render_config c;
    memset(&c, 0, sizeof c);
    c.cell_w = 8;
    c.cell_h = 16;
    c.pad_left = c.pad_right = c.pad_top = c.pad_bottom = 4;

    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    /* 8 + 40*8 wide, 8 + 10*16 tall. */
    tt_layout_apply(&l, area_of(8 + 40 * 8, 8 + 10 * 16));
    int cols = 0, rows = 0;
    tt_render_grid_for(&c, l.root->rect.w, l.root->rect.h, &cols, &rows);
    ASSERT_EQ(40, cols);
    ASSERT_EQ(10, rows);

    /* Split it: each half loses its share of the rule and of a second
     * pane's padding, so two halves hold fewer columns than one whole. */
    tt_node *root = l.root;
    tt_layout_split(&l, root, TT_SPLIT_VERT, &pane_b);
    int lcols = 0, rcols = 0;
    tt_render_grid_for(&c, root->a->rect.w, root->a->rect.h, &lcols, &rows);
    tt_render_grid_for(&c, root->b->rect.w, root->b->rect.h, &rcols, &rows);
    ASSERT_EQ(19, lcols); /* (159 - 8) / 8 */
    ASSERT_EQ(19, rcols);
    ASSERT_EQ(10, rows); /* height is untouched by a vertical split */
    tt_layout_free(&l, NULL, NULL);
    PASS();
}

/* tt_layout_free hands every leaf's payload back, exactly once. */
static void count_drop(void *user, void *ctx) {
    (void)user;
    (*(int *)ctx)++;
}

TEST freeing_drops_every_leaf_payload_once(void) {
    tt_layout l;
    tt_layout_init(&l, 1, &pane_a);
    tt_layout_apply(&l, area_of(800, 600));
    tt_node *b = tt_layout_split(&l, l.root, TT_SPLIT_VERT, &pane_b);
    tt_layout_split(&l, b, TT_SPLIT_HORZ, &pane_c);
    tt_layout_split(&l, l.root->a, TT_SPLIT_HORZ, &pane_d);
    ASSERT_EQ(4, tt_layout_count(&l));
    int dropped = 0;
    tt_layout_free(&l, count_drop, &dropped);
    ASSERT_EQ(4, dropped);
    PASS();
}

SUITE(layout_suite) {
    RUN_TEST(one_pane_fills_the_area);
    RUN_TEST(vertical_split_puts_the_new_pane_on_the_right);
    RUN_TEST(horizontal_split_puts_the_new_pane_below);
    RUN_TEST(odd_sizes_never_lose_or_overlap_a_pixel);
    RUN_TEST(nested_splits_subdivide_only_their_own_rect);
    RUN_TEST(closing_a_pane_gives_its_space_to_the_sibling);
    RUN_TEST(closing_the_last_pane_is_refused);
    RUN_TEST(closing_a_nested_pane_promotes_its_subtree);
    RUN_TEST(hit_testing_finds_the_pane_and_skips_the_divider);
    RUN_TEST(focus_moves_to_the_neighbour_in_that_direction);
    RUN_TEST(cycling_walks_every_pane_in_leaf_order_and_wraps);
    RUN_TEST(cycling_a_single_pane_stays_put);
    RUN_TEST(cycling_after_a_close_skips_the_pane_that_left);
    RUN_TEST(pane_rects_become_cols_and_rows);
    RUN_TEST(freeing_drops_every_leaf_payload_once);
}
