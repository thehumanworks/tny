from .totals import cart_total_eur


def invoice_total(lines, rate):
    return cart_total_eur(lines, rate)
