/* test_tabs.c — bounded, platform-free tab ordering and active selection. */
#include "greatest.h"
#include "ui/tabs.h"

static char payload[TT_TABS_MAX + 1];

TEST tabs_start_empty(void) {
    tt_tabs tabs;
    tt_tabs_init(&tabs);
    ASSERT_EQ(0, tt_tabs_count(&tabs));
    ASSERT_EQ(-1, tt_tabs_active_index(&tabs));
    ASSERT_EQ(NULL, tt_tabs_active(&tabs));
    ASSERT_EQ(NULL, tt_tabs_at(&tabs, 0));
    PASS();
}

TEST adding_appends_and_selects_the_new_tab(void) {
    tt_tabs tabs;
    tt_tabs_init(&tabs);
    ASSERT_EQ(0, tt_tabs_add(&tabs, &payload[0]));
    ASSERT_EQ(1, tt_tabs_add(&tabs, &payload[1]));
    ASSERT_EQ(2, tt_tabs_count(&tabs));
    ASSERT_EQ(1, tt_tabs_active_index(&tabs));
    ASSERT_EQ(&payload[0], tt_tabs_at(&tabs, 0));
    ASSERT_EQ(&payload[1], tt_tabs_active(&tabs));
    PASS();
}

TEST collection_is_bounded_at_sixteen_tabs(void) {
    tt_tabs tabs;
    tt_tabs_init(&tabs);
    for (int i = 0; i < TT_TABS_MAX; i++) ASSERT_EQ(i, tt_tabs_add(&tabs, &payload[i]));
    ASSERT_EQ(-1, tt_tabs_add(&tabs, &payload[TT_TABS_MAX]));
    ASSERT_EQ(TT_TABS_MAX, tt_tabs_count(&tabs));
    ASSERT_EQ(TT_TABS_MAX - 1, tt_tabs_active_index(&tabs));
    ASSERT_EQ(&payload[TT_TABS_MAX - 1], tt_tabs_active(&tabs));
    PASS();
}

TEST selecting_rejects_invalid_indices_without_moving(void) {
    tt_tabs tabs;
    tt_tabs_init(&tabs);
    tt_tabs_add(&tabs, &payload[0]);
    tt_tabs_add(&tabs, &payload[1]);
    ASSERT(tt_tabs_select(&tabs, 0));
    ASSERT_FALSE(tt_tabs_select(&tabs, -1));
    ASSERT_FALSE(tt_tabs_select(&tabs, 2));
    ASSERT_EQ(0, tt_tabs_active_index(&tabs));
    ASSERT_EQ(&payload[0], tt_tabs_active(&tabs));
    PASS();
}

TEST cycling_wraps_in_both_directions(void) {
    tt_tabs tabs;
    tt_tabs_init(&tabs);
    for (int i = 0; i < 3; i++) tt_tabs_add(&tabs, &payload[i]);
    ASSERT_EQ(&payload[0], tt_tabs_cycle(&tabs, 1));
    ASSERT_EQ(&payload[2], tt_tabs_cycle(&tabs, -1));
    ASSERT_EQ(&payload[1], tt_tabs_cycle(&tabs, -1));
    ASSERT_EQ(&payload[1], tt_tabs_cycle(&tabs, 0));
    PASS();
}

TEST removing_the_active_middle_tab_selects_its_successor(void) {
    tt_tabs tabs;
    tt_tabs_init(&tabs);
    for (int i = 0; i < 3; i++) tt_tabs_add(&tabs, &payload[i]);
    tt_tabs_select(&tabs, 1);
    ASSERT_EQ(&payload[1], tt_tabs_remove(&tabs, 1));
    ASSERT_EQ(2, tt_tabs_count(&tabs));
    ASSERT_EQ(1, tt_tabs_active_index(&tabs));
    ASSERT_EQ(&payload[2], tt_tabs_active(&tabs));
    PASS();
}

TEST removing_the_active_last_tab_selects_its_predecessor(void) {
    tt_tabs tabs;
    tt_tabs_init(&tabs);
    for (int i = 0; i < 3; i++) tt_tabs_add(&tabs, &payload[i]);
    ASSERT_EQ(&payload[2], tt_tabs_remove(&tabs, 2));
    ASSERT_EQ(1, tt_tabs_active_index(&tabs));
    ASSERT_EQ(&payload[1], tt_tabs_active(&tabs));
    PASS();
}

TEST removing_before_the_active_tab_preserves_the_active_payload(void) {
    tt_tabs tabs;
    tt_tabs_init(&tabs);
    for (int i = 0; i < 4; i++) tt_tabs_add(&tabs, &payload[i]);
    tt_tabs_select(&tabs, 2);
    ASSERT_EQ(&payload[0], tt_tabs_remove(&tabs, 0));
    ASSERT_EQ(1, tt_tabs_active_index(&tabs));
    ASSERT_EQ(&payload[2], tt_tabs_active(&tabs));
    ASSERT_EQ(&payload[3], tt_tabs_at(&tabs, 2));
    PASS();
}

TEST removing_after_the_active_tab_does_not_move_it(void) {
    tt_tabs tabs;
    tt_tabs_init(&tabs);
    for (int i = 0; i < 3; i++) tt_tabs_add(&tabs, &payload[i]);
    tt_tabs_select(&tabs, 0);
    ASSERT_EQ(&payload[2], tt_tabs_remove(&tabs, 2));
    ASSERT_EQ(0, tt_tabs_active_index(&tabs));
    ASSERT_EQ(&payload[0], tt_tabs_active(&tabs));
    PASS();
}

TEST removing_the_only_tab_returns_to_the_empty_state(void) {
    tt_tabs tabs;
    tt_tabs_init(&tabs);
    tt_tabs_add(&tabs, &payload[0]);
    ASSERT_EQ(&payload[0], tt_tabs_remove(&tabs, 0));
    ASSERT_EQ(0, tt_tabs_count(&tabs));
    ASSERT_EQ(-1, tt_tabs_active_index(&tabs));
    ASSERT_EQ(NULL, tt_tabs_active(&tabs));
    ASSERT_EQ(NULL, tt_tabs_cycle(&tabs, 1));
    ASSERT_EQ(NULL, tt_tabs_remove(&tabs, 0));
    PASS();
}

SUITE(tabs_suite) {
    RUN_TEST(tabs_start_empty);
    RUN_TEST(adding_appends_and_selects_the_new_tab);
    RUN_TEST(collection_is_bounded_at_sixteen_tabs);
    RUN_TEST(selecting_rejects_invalid_indices_without_moving);
    RUN_TEST(cycling_wraps_in_both_directions);
    RUN_TEST(removing_the_active_middle_tab_selects_its_successor);
    RUN_TEST(removing_the_active_last_tab_selects_its_predecessor);
    RUN_TEST(removing_before_the_active_tab_preserves_the_active_payload);
    RUN_TEST(removing_after_the_active_tab_does_not_move_it);
    RUN_TEST(removing_the_only_tab_returns_to_the_empty_state);
}
