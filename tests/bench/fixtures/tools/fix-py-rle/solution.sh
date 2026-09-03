#!/bin/sh
# Reference solution: proves check.sh is satisfiable and drives the
# bench harness's scripted --mock trajectory. Never copied into the
# scratch workspace a model sees.
set -e
python3 - << 'TNYSUB'
s = open("mod.py").read()
assert '            cur, n = ch, 1\n    return out' in s
open("mod.py", "w").write(s.replace('            cur, n = ch, 1\n    return out', '            cur, n = ch, 1\n    out.append((cur, n))\n    return out'))
TNYSUB
