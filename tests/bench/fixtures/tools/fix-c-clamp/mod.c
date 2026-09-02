#include "mod.h"

/* Clamp v into [lo, hi]. */
int clamp(int v, int lo, int hi) {
    if (v < lo) return hi;
    if (v > hi) return lo;
    return v;
}
