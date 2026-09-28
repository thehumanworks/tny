import sqlite3

from report import customer_report

with sqlite3.connect("report.db") as conn:
    assert customer_report(conn, 1) == {
        "billed_cents": 1200,
        "paid_cents": 550,
        "due_cents": 650,
    }
