from .invoice import invoice_total


def calculate(lines, rate):
    return str(invoice_total(lines, rate))
