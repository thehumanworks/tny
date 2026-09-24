#!/bin/sh
set -eu
python3 - "$1" << 'INNER'
import sqlite3
import sys
from pathlib import Path
w = Path(sys.argv[1])
db = w / "report.db"
if db.exists():
    db.unlink()
with sqlite3.connect(db) as conn:
    conn.executescript((w / "schema.sql").read_text())
    conn.executemany("INSERT INTO customers VALUES (?,?)", [(1, "Acme"), (2, "Bee")])
    conn.executemany("INSERT INTO orders VALUES (?,?)", [(10, 1), (11, 1), (20, 2)])
    conn.executemany("INSERT INTO order_lines VALUES (?,?,?,?)", [(1, 10, 2, 250), (2, 10, 1, 300), (3, 11, 1, 100), (4, 20, 1, 900)])
    conn.executemany("INSERT INTO payments VALUES (?,?,?)", [(1, 10, 200), (2, 10, 150), (3, 11, 50)])
INNER
