/* tabs.h — bounded, platform-free tab ordering and selection.
 *
 * A tab payload is opaque to this module. In the GUI it owns one split-pane
 * layout; tests can use tagged pointers without sessions or a window. */
#ifndef TNYTTY_UI_TABS_H
#define TNYTTY_UI_TABS_H

#include <stdbool.h>

#define TT_TABS_MAX 16

typedef struct {
    void *items[TT_TABS_MAX];
    int count;
    int active; /* -1 while empty */
} tt_tabs;

void tt_tabs_init(tt_tabs *tabs);

/* Append payload and select it. Returns its index, or -1 when full. */
int tt_tabs_add(tt_tabs *tabs, void *payload);

/* Select index. Returns false for an out-of-range index. */
bool tt_tabs_select(tt_tabs *tabs, int index);

/* Select the next (step > 0) or previous (step < 0) tab, wrapping. A zero
 * step leaves the selection unchanged. Returns the selected payload, or NULL
 * while empty. */
void *tt_tabs_cycle(tt_tabs *tabs, int step);

/* Remove and return index's payload. If it was active, the tab that slid into
 * the same index becomes active, falling back to the previous tab at the end.
 * Removing a tab before the active tab preserves the same active payload. */
void *tt_tabs_remove(tt_tabs *tabs, int index);

void *tt_tabs_active(const tt_tabs *tabs);
void *tt_tabs_at(const tt_tabs *tabs, int index);
int tt_tabs_count(const tt_tabs *tabs);
int tt_tabs_active_index(const tt_tabs *tabs);

#endif
