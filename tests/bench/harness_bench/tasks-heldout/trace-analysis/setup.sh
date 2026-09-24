#!/bin/sh
set -eu
python3 - "$1" << 'INNER'
import json
import random
import sys
from pathlib import Path

out = Path(sys.argv[1]) / "trace.ndjson"
rng = random.Random(4812)
endpoints = ["/v2/search", "/v2/export", "/v2/reconcile", "/v2/import", "/v2/approve"]
padding = "x" * 24
with out.open("w") as stream:
    for index in range(160000):
        trace_id = f"t{index:06x}"
        root_id = f"r{index:06x}"
        child_id = f"c{index:06x}"
        endpoint = endpoints[rng.randrange(len(endpoints))]
        status = "ERROR" if rng.randrange(5) == 0 else "OK"
        duration = rng.randrange(1200, 5001)
        child_duration = rng.randrange(100, 1001)
        root = {"trace_id": trace_id, "span_id": root_id, "parent_id": None, "kind": "request", "endpoint": endpoint, "status": status, "duration_us": duration, "attrs": padding}
        child = {"trace_id": trace_id, "span_id": child_id, "parent_id": root_id, "kind": "db", "duration_us": child_duration, "attrs": padding}
        records = (child, root) if index % 2 else (root, child)
        for record in records:
            stream.write(json.dumps(record, separators=(",", ":")) + "\n")
INNER
