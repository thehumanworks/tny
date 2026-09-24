#!/bin/sh
set -eu
python3 - "$1" << 'INNER'
from pathlib import Path
import sys
p=Path(sys.argv[1])/'services.log'
with p.open('w') as out:
    for i in range(300000):
        ts=f"2026-04-07T13:{(i//60)%60:02d}:{i%60:02d}.{i%1000:03d}Z"
        service=('gateway','queue','auth','inventory','payments')[i%5]
        if i==173841:
            out.write('2026-04-07T13:42:17.381Z inventory ERROR request_id=req-7f3a2c91 event=origin_failure cause=pool_generation_mismatch all_new_connections_rejected\n')
        elif 173842<=i<175842 and i%3==0:
            out.write(f'{ts} {service} ERROR request_id=req-{i:08x} event=retry_exhausted parent=req-7f3a2c91 status=503 attempt=4 downstream_cascade\n')
        else:
            out.write(f'{ts} {service} INFO request_id=req-{i:08x} event=heartbeat status=ok retry={i%4} node=n{i%37:02d} route=/v2/items latency_ms={i%89} periodic_health_check\n')
INNER
