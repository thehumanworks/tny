#!/bin/sh
set -eu
python3 - << 'PY'
import csv
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path

out = Path("evidence")
out.mkdir(exist_ok=True)
base = datetime(2026, 6, 14, 10, tzinfo=timezone.utc)
logs = [(out / f"requests-{index:02d}.jsonl").open("w") for index in range(4)]
metrics = [(out / f"metrics-{index:02d}.csv").open("w") for index in range(8)]
writers = [csv.writer(handle) for handle in metrics]
for writer in writers:
    writer.writerow(["timestamp", "tenant", "pool", "inflight", "capacity", "queue_ms"])
try:
    for second in range(6000):
        stamp = (base + timedelta(seconds=second)).strftime("%Y-%m-%dT%H:%M:%SZ")
        incident = 1200 <= second < 3000
        peak = 1800 <= second < 2100
        for slot in range(100):
            number = second * 100 + slot
            tenant_number = slot % 24
            tenant = f"tenant-{tenant_number:02d}"
            premium = tenant_number % 4 == 0
            failed = incident and premium and (peak or slot % 8 == 0)
            entry = {
                "ts": stamp, "request_id": f"req-{number:06d}",
                "tenant": tenant, "tier": "premium" if premium else "standard",
                "pool": "default-20" if premium else "standard-20",
                "status": 503 if failed else 200,
                "latency_ms": 1500 + slot if failed else 18 + slot % 9,
                "error": "pool_saturated" if failed else None,
            }
            logs[tenant_number % 4].write(json.dumps(entry, separators=(",", ":")) + "\n")
        if second % 2 == 0:
            for tenant_number in range(24):
                premium = tenant_number % 4 == 0
                capacity = 20
                inflight = 50 if incident and premium else 8 + tenant_number % 5
                writers[tenant_number % 8].writerow([
                    stamp, f"tenant-{tenant_number:02d}",
                    "default-20" if premium else "standard-20",
                    inflight, capacity, 1400 if incident and premium else 3,
                ])
finally:
    for handle in logs + metrics:
        handle.close()
PY
