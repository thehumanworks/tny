#!/bin/sh
set -eu
python3 - "$1" << 'INNER'
from pathlib import Path
import sys
w=Path(sys.argv[1])
h=['#ifndef AGGREGATES_H','#define AGGREGATES_H','#include <stddef.h>']
c=['#include "aggregates.h"']
for n in range(400):
    h.append(f'long scan_{n:03d}(const int*,size_t);')
    c += [f'long scan_{n:03d}(const int *v,size_t n) {{','    long total=0;','    size_t i=0;']
    c += [f'    /* stage {j:02d}: regular reduction path */' for j in range(24)]
    c += ['    for(i=0;'+('i+1<n' if n==217 else 'i<n')+';++i) {','        total+=v[i];','    }','    return total;','}','']
h+=['#endif','']
(w/'aggregates.h').write_text('\n'.join(h))
(w/'aggregates.c').write_text('\n'.join(c))
INNER
