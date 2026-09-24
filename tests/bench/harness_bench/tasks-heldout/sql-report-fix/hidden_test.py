import json
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

w = Path(sys.argv[1])
sys.path.insert(0, str(w))
from report import customer_report

with sqlite3.connect(w / "report.db") as conn:
    assert customer_report(conn, 1) == {
        "billed_cents": 1200,
        "paid_cents": 550,
        "due_cents": 650,
    }
    assert customer_report(conn, 2) == {
        "billed_cents": 900,
        "paid_cents": 0,
        "due_cents": 900,
    }
    assert customer_report(conn, 999) == {
        "billed_cents": 0,
        "paid_cents": 0,
        "due_cents": 0,
    }
cli = subprocess.run(
    [sys.executable, "report.py", "report.db", "1"],
    cwd=w,
    text=True,
    capture_output=True,
    check=True,
)
assert json.loads(cli.stdout) == {
    "billed_cents": 1200,
    "paid_cents": 550,
    "due_cents": 650,
}
with tempfile.TemporaryDirectory() as tmp:
    with sqlite3.connect(Path(tmp) / "extra.db") as conn:
        conn.executescript((w / "schema.sql").read_text())
        conn.executemany("INSERT INTO orders VALUES (?,?)", [(1, 7), (2, 7), (3, 8)])
        conn.executemany(
            "INSERT INTO order_lines VALUES (?,?,?,?)",
            [
                (1, 1, 3, 17),
                (2, 1, 2, 25),
                (3, 2, 1, 49),
                (4, 3, 1, 999),
                (5, 1, 2, 25),
            ],
        )
        conn.executemany(
            "INSERT INTO payments VALUES (?,?,?)",
            [(1, 1, 31), (2, 1, 20), (3, 2, 10), (4, 1, 20)],
        )
        assert customer_report(conn, 7) == {
            "billed_cents": 200,
            "paid_cents": 81,
            "due_cents": 119,
        }
