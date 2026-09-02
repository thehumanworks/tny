#!/bin/sh
# Reference solution: proves check.sh is satisfiable and drives the
# bench harness's scripted --mock trajectory. Never copied into the
# scratch workspace a model sees.
set -e
python3 - << 'TNYREN'
import glob, re
for e in "py".split(","):
    for f in glob.glob("*." + e):
        s = open(f).read()
        open(f, "w").write(re.sub(r"\btok\b", "tokenize", s))
TNYREN
