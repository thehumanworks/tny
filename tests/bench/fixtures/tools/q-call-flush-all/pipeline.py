import hashlib

MAX_RETRIES = 5


def digest(data):
    return hashlib.sha256(data).hexdigest()


def compute_total(rows):
    total = 0
    for row in rows:
        total += row['amount']
    return total
