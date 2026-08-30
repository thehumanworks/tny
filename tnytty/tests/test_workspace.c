#include "greatest.h"
#include "ui/workspace.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void sample(tt_workspace *ws) {
    tt_workspace_init(ws);
    ws->active = 0;
    ws->tab_count = 1;
    tt_workspace_tab *tab = &ws->tabs[0];
    tab->root = 0;
    tab->focus = 2;
    tab->node_count = 3;
    tab->nodes[0] = (tt_workspace_node){TT_SPLIT_VERT, 500, 1, 2, ""};
    tab->nodes[1] = (tt_workspace_node){TT_SPLIT_LEAF, 500, -1, -1, "1234abcd"};
    tab->nodes[2] = (tt_workspace_node){TT_SPLIT_LEAF, 500, -1, -1, "deadbeef"};
}

TEST workspace_round_trip_and_permissions(void) {
    char dir[] = "/tmp/tnytty-workspace-XXXXXX";
    ASSERT(mkdtemp(dir) != NULL);
    char path[256];
    snprintf(path, sizeof path, "%s/state/nested/workspace", dir);
    tt_workspace before, after;
    sample(&before);
    char err[128];
    ASSERT_EQ(0, tt_workspace_save(path, &before, err, sizeof err));
    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(0, (int)(st.st_mode & 077));
    ASSERT_EQ(0, tt_workspace_load(path, &after, err, sizeof err));
    ASSERT_EQ(before.tab_count, after.tab_count);
    ASSERT_EQ(before.active, after.active);
    ASSERT_EQ(3, after.tabs[0].node_count);
    ASSERT_EQ(TT_SPLIT_VERT, after.tabs[0].nodes[0].dir);
    ASSERT_STR_EQ("deadbeef", after.tabs[0].nodes[2].session_id);
    unlink(path);
    char nested[256], state[256];
    snprintf(nested, sizeof nested, "%s/state/nested", dir);
    snprintf(state, sizeof state, "%s/state", dir);
    rmdir(nested);
    rmdir(state);
    rmdir(dir);
    PASS();
}

TEST workspace_missing_is_not_an_error(void) {
    tt_workspace ws;
    char err[64];
    ASSERT_EQ(1, tt_workspace_load("/tmp/tnytty-no-such-workspace", &ws, err, sizeof err));
    PASS();
}

TEST workspace_rejects_corrupt_and_trailing_data(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/tnytty-workspace-bad-%ld", (long)getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT(fd >= 0);
    ASSERT_EQ(3, write(fd, "bad", 3));
    close(fd);
    tt_workspace ws;
    char err[128];
    ASSERT_EQ(-1, tt_workspace_load(path, &ws, err, sizeof err));

    sample(&ws);
    ASSERT_EQ(0, tt_workspace_save(path, &ws, err, sizeof err));
    fd = open(path, O_WRONLY | O_APPEND);
    ASSERT(fd >= 0);
    ASSERT_EQ(1, write(fd, "x", 1));
    close(fd);
    ASSERT_EQ(-1, tt_workspace_load(path, &ws, err, sizeof err));
    unlink(path);
    PASS();
}

TEST workspace_validation_rejects_cycles_and_bad_ids(void) {
    tt_workspace ws;
    char err[128];
    sample(&ws);
    ws.tabs[0].nodes[0].a = 0;
    ASSERT_EQ(-1, tt_workspace_validate(&ws, err, sizeof err));
    sample(&ws);
    snprintf(ws.tabs[0].nodes[1].session_id, 9, "%s", "NOTHEX!!");
    ASSERT_EQ(-1, tt_workspace_validate(&ws, err, sizeof err));
    PASS();
}

SUITE(workspace_suite) {
    RUN_TEST(workspace_round_trip_and_permissions);
    RUN_TEST(workspace_missing_is_not_an_error);
    RUN_TEST(workspace_rejects_corrupt_and_trailing_data);
    RUN_TEST(workspace_validation_rejects_cycles_and_bad_ids);
}
