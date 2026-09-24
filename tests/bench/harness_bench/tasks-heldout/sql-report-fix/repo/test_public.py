import sqlite3

from report import customer_report

with sqlite3.connect("report.db") as conn:
    assert customer_report(conn, 1) == {
        "billed_cents": 900,
        "paid_cents": 400,
        "due_cents": 500,
    }
