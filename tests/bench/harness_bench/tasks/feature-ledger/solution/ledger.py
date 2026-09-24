import sqlite3


class Ledger:
    def __init__(self, path):
        self.db = sqlite3.connect(path, timeout=30, isolation_level=None)
        self.db.execute("PRAGMA busy_timeout=30000")
        self.db.execute(
            "CREATE TABLE IF NOT EXISTS stock (sku TEXT PRIMARY KEY, amount INTEGER NOT NULL)"
        )
        self.db.execute(
            "CREATE TABLE IF NOT EXISTS events (id TEXT PRIMARY KEY, sku TEXT NOT NULL, delta INTEGER NOT NULL, result INTEGER NOT NULL)"
        )

    def apply(self, event_id, sku, delta):
        if (
            not isinstance(event_id, str)
            or not 1 <= len(event_id) <= 64
            or not isinstance(sku, str)
            or not 1 <= len(sku) <= 64
            or type(delta) is not int
        ):
            raise ValueError("invalid event")
        self.db.execute("BEGIN IMMEDIATE")
        try:
            prior = self.db.execute(
                "SELECT sku,delta,result FROM events WHERE id=?", (event_id,)
            ).fetchone()
            if prior is not None:
                if prior[:2] != (sku, delta):
                    raise ValueError("conflicting event")
                result = prior[2]
            else:
                old = self.db.execute(
                    "SELECT amount FROM stock WHERE sku=?", (sku,)
                ).fetchone()
                result = (old[0] if old else 0) + delta
                if result < 0:
                    raise ValueError("negative stock")
                self.db.execute(
                    "INSERT INTO stock(sku,amount) VALUES(?,?) ON CONFLICT(sku) DO UPDATE SET amount=excluded.amount",
                    (sku, result),
                )
                self.db.execute(
                    "INSERT INTO events(id,sku,delta,result) VALUES(?,?,?,?)",
                    (event_id, sku, delta, result),
                )
            self.db.execute("COMMIT")
            return result
        except BaseException:
            self.db.execute("ROLLBACK")
            raise

    def snapshot(self):
        return dict(self.db.execute("SELECT sku,amount FROM stock"))

    def close(self):
        self.db.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
