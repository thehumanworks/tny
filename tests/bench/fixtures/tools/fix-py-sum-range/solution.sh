#!/bin/sh
# Reference solution: proves check.sh is satisfiable and drives the
# bench harness's scripted --mock trajectory. Never copied into the
# scratch workspace a model sees.
set -e
python3 - << 'TNYSUB'
s = open("mod.py").read()
assert 'for i in range(a, b):' in s
open("mod.py", "w").write(s.replace('for i in range(a, b):', 'for i in range(a, b + 1):'))
TNYSUB
