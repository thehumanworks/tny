#!/bin/sh
# Reference solution: proves check.sh is satisfiable and drives the
# bench harness's scripted --mock trajectory. Never copied into the
# scratch workspace a model sees.
set -e
python3 - << 'TNYSUB'
s = open("mod.py").read()
assert 'xs[i:i + n - 1]' in s
open("mod.py", "w").write(s.replace('xs[i:i + n - 1]', 'xs[i:i + n]'))
TNYSUB
