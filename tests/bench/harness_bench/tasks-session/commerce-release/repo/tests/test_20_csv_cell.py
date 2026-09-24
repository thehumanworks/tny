import unittest

from commerceops import csv_cell


class TestCsvCell(unittest.TestCase):
    def test_regression(self):
        self.assertEqual(csv_cell.encode('A,"B"'), '"A,""B"""')
