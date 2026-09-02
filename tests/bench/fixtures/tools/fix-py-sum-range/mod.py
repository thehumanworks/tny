def sum_range(a, b):
    """Sum every integer from a to b inclusive."""
    total = 0
    for i in range(a, b):
        total += i
    return total
