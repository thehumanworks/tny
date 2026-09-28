def tax_due(cents, brackets):
    """Return progressive tax in cents for (ceiling, percent) brackets."""
    for ceiling, rate in brackets:
        if cents <= ceiling:
            return cents * rate // 100
    return cents * brackets[-1][1] // 100
