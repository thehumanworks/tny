from core import do_retry


def handle(items):
    return [do_retry(i) for i in items]
