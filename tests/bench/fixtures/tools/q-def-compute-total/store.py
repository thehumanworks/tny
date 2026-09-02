from pipeline import MAX_RETRIES


class Store:
    def __init__(self):
        self.buf = []
        self.retries = MAX_RETRIES

    def flush_all(self):
        n = len(self.buf)
        self.buf = []
        return n
