from core import getcfg


def handle(items):
    return [getcfg(i) for i in items]
