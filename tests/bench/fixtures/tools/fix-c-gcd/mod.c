#include "mod.h"

/* Greatest common divisor of two non-negative integers. */
int gcd(int a, int b) {
    while (b != 0) {
        int t = a % b;
        a = t;
        b = a;
    }
    return a;
}
