#!/bin/sh
set -eu
# Called with cwd at the copied workspace and no arguments.
mkdir -p fixtures
python3 - << 'PY'
from pathlib import Path

path = Path("fixtures/settlements.csv")
with path.open("w") as stream:
    stream.write("transaction_id,kind,cents\n")
    for index in range(12000):
        kind = "refund" if index % 19 == 0 else "charge"
        stream.write(f"txn-{index:05d},{kind},{100 + index % 900}\n")
PY
