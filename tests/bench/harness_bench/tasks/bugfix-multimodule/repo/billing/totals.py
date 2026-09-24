from .currency import to_eur


def cart_total_eur(lines, rate):
    return sum(
        (to_eur(x["usd_price"] * x["quantity"], rate) for x in lines), to_eur(0, rate)
    )
