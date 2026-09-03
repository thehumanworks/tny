#!/bin/sh
# Reference solution: proves check.sh is satisfiable and drives the
# bench harness's scripted --mock trajectory. Never copied into the
# scratch workspace a model sees.
set -e
python3 - << 'TNYSUB'
s = open("mod.py").read()
assert "line.split('=')" in s
open("mod.py", "w").write(s.replace("line.split('=')", "line.split('=', 1)"))
TNYSUB
