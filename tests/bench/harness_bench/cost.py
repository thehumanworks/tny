"""List-price comparison units for provider-reported Responses usage."""

PRICE_DATE = "2026-09-24"
PRICES = {
    "gpt-6-astra": (10.0, 1.0, 12.5, 50.0),
    "gpt-6-sol": (2.0, 0.2, 2.5, 10.0),
    "gpt-6-luna": (0.1, 0.01, 0.125, 0.5),
    "gpt-5.6-terra": (2.0, 0.2, 2.5, 12.0),
    "gpt-5.6-luna": (0.2, 0.02, 0.25, 1.2),
}


def request_cost(row, model):
    """Return (ITE, USD, uncached input) or nulls for unavailable usage/prices."""
    prices = PRICES.get(model)
    fields = ("input_tokens", "cached_input_tokens", "output_tokens")
    if not prices or any(row.get(key) is None for key in fields):
        return None, None, None
    written = row.get("cache_write_tokens") or 0
    uncached = row["input_tokens"] - row["cached_input_tokens"] - written
    if uncached < 0:
        return None, None, None
    multiplier = 2 if row["input_tokens"] > 272_000 else 1
    input_weight, cached_weight, write_weight, output_weight = prices
    ite_output_weight = 5 if model.startswith("gpt-6-") else 6
    ite = (
        multiplier * (uncached + row["cached_input_tokens"] * 0.1 + written * 1.25)
        + row["output_tokens"] * ite_output_weight
    )
    usd = (
        multiplier
        * (
            uncached * input_weight
            + row["cached_input_tokens"] * cached_weight
            + written * write_weight
        )
        + row["output_tokens"] * output_weight
    ) / 1_000_000
    return ite, usd, uncached
