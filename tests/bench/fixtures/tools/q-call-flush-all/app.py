from pipeline import compute_total
from store import Store


def run(rows):
    s = Store()
    s.buf = list(rows)
    # TODO: report partial failures to the caller
    s.flush_all()
    return compute_total(rows)
