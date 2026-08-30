#include "ui/tabs.h"

#include <stddef.h>
#include <string.h>

void tt_tabs_init(tt_tabs *tabs) {
    memset(tabs, 0, sizeof *tabs);
    tabs->active = -1;
}

int tt_tabs_add(tt_tabs *tabs, void *payload) {
    if (!tabs || tabs->count >= TT_TABS_MAX) return -1;
    int index = tabs->count++;
    tabs->items[index] = payload;
    tabs->active = index;
    return index;
}

bool tt_tabs_select(tt_tabs *tabs, int index) {
    if (!tabs || index < 0 || index >= tabs->count) return false;
    tabs->active = index;
    return true;
}

void *tt_tabs_cycle(tt_tabs *tabs, int step) {
    if (!tabs || tabs->count == 0) return NULL;
    if (step > 0) tabs->active = (tabs->active + 1) % tabs->count;
    else if (step < 0) tabs->active = (tabs->active + tabs->count - 1) % tabs->count;
    return tabs->items[tabs->active];
}

void *tt_tabs_remove(tt_tabs *tabs, int index) {
    if (!tabs || index < 0 || index >= tabs->count) return NULL;
    void *payload = tabs->items[index];
    int old_active = tabs->active;
    int tail = tabs->count - index - 1;
    if (tail > 0)
        memmove(&tabs->items[index], &tabs->items[index + 1], (size_t)tail * sizeof *tabs->items);
    tabs->items[--tabs->count] = NULL;

    if (tabs->count == 0) tabs->active = -1;
    else if (index < old_active) tabs->active = old_active - 1;
    else if (index == old_active && old_active >= tabs->count) tabs->active = tabs->count - 1;
    return payload;
}

void *tt_tabs_active(const tt_tabs *tabs) {
    return tabs && tabs->active >= 0 && tabs->active < tabs->count ? tabs->items[tabs->active] : NULL;
}

void *tt_tabs_at(const tt_tabs *tabs, int index) {
    return tabs && index >= 0 && index < tabs->count ? tabs->items[index] : NULL;
}

int tt_tabs_count(const tt_tabs *tabs) { return tabs ? tabs->count : 0; }

int tt_tabs_active_index(const tt_tabs *tabs) { return tabs ? tabs->active : -1; }
