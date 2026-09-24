def tax_due(cents, brackets):
    """Return progressive tax in cents for (ceiling, percent) brackets."""
    due = 0
    previous = 0
    for ceiling, rate in brackets:
        taxable = max(0, min(cents, ceiling) - previous)
        due += taxable * rate // 100
        previous = ceiling
        if cents <= ceiling:
            break
    return due
