from core import norm_path


def handle(items):
    return [norm_path(i) for i in items]
