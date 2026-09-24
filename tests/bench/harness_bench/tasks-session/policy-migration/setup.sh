#!/bin/sh
set -eu
python3 - << 'PY'
from pathlib import Path
Path('fixtures').mkdir(exist_ok=True)
Path('fixtures/records.tsv').write_text('scope\tidentifier\trevision\tvalue\n' + ''.join(
    f'tenant-{i % 17}\tbilling:mode\t{i}\tmode-{i % 5}\n' for i in range(2000)))
PY
mkdir -p evidence
python3 - << 'PY'
import json
from pathlib import Path

domains = (
    "billing", "shipping", "catalog", "inventory", "fraud", "returns",
    "notifications", "tax", "discounts", "fulfillment", "identity",
    "search", "recommendations", "payments", "invoicing", "subscriptions",
    "support", "analytics", "experiments", "localization", "compliance",
    "audit", "routing", "quotas", "rate_limits", "webhooks", "exports",
    "imports", "scheduling", "settlements",
)
for shard in range(3):
    with (Path("evidence") / f"resolution-trace-{shard}.jsonl").open("w") as stream:
        for index in range(15000):
            stream.write(json.dumps({
                "request": f"migration-{shard}-{index:05d}",
                "domain": domains[(index + shard * 11) % len(domains)],
                "tenant": f"tenant-{index % 41:02d}",
                "revision": index % 17,
                "state": ("deleted", "expired", "null", "found")[index % 4],
                "read": "historical" if index % 3 == 0 else "head",
                "sequence": index,
            }, separators=(",", ":")) + "\n")
PY
