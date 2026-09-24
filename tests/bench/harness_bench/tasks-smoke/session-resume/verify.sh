#!/bin/sh
set -eu
workspace=$1
: "$2"
python3 - "$workspace" << 'PY'
import sys
from pathlib import Path

actual = (Path(sys.argv[1]) / "session.txt").read_text().splitlines()
assert actual == ["cedar", "birch"], actual
print("pass: both session turns completed")
PY
