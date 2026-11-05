#!/usr/bin/env python3
"""Optional benchmark-only Monty worker adapter; no runtime dependency for tny."""

import sys

from pydantic_monty import Monty

with Monty() as pool:
    with pool.checkout() as cell:
        print(cell.feed_run(sys.stdin.read()))
