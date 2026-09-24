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
mkdir -p evidence
python3 - << 'PY'
import json
from pathlib import Path

modules = (
    "tax", "discount", "inventory", "coupon", "zone", "invoice", "retry",
    "postcode", "currency", "reconcile", "event_order", "sla", "override",
    "notifications", "pagination", "allocation", "ledger", "scopes",
    "percentile", "csv_cell",
)
for shard in range(3):
    with (Path("evidence") / f"release-trace-{shard}.jsonl").open("w") as stream:
        for index in range(15000):
            module = modules[(index + shard * 7) % len(modules)]
            stream.write(json.dumps({
                "request": f"rel-{shard}-{index:05d}",
                "module": module,
                "tenant": f"tenant-{index % 37:02d}",
                "case": "boundary" if index % 29 == 0 else "routine",
                "observed": "regression" if index % 29 == 0 else "ok",
                "expected": "contract" if index % 29 == 0 else "ok",
                "sequence": index,
            }, separators=(",", ":")) + "\n")
PY
