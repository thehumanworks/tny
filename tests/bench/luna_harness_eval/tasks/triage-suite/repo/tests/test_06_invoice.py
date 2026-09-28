import unittest

from commerceops import invoice


class TestInvoice(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(invoice.next_number(["INV-9", "INV-10", "INV-2"]), "INV-11")
