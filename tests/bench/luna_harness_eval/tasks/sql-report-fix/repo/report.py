import argparse
import json
import sqlite3


def customer_report(conn, customer_id):
    billed, paid = conn.execute(
        "SELECT COALESCE(SUM(l.qty*l.unit_cents),0), COALESCE(SUM(p.amount_cents),0) "
        "FROM orders o LEFT JOIN order_lines l ON l.order_id=o.id "
        "LEFT JOIN payments p ON p.order_id=o.id WHERE o.customer_id=?",
        (customer_id,),
    ).fetchone()
    return {"billed_cents": billed, "paid_cents": paid, "due_cents": billed - paid}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("database")
    parser.add_argument("customer_id", type=int)
    args = parser.parse_args()
    with sqlite3.connect(args.database) as conn:
        print(json.dumps(customer_report(conn, args.customer_id)))


if __name__ == "__main__":
    main()
