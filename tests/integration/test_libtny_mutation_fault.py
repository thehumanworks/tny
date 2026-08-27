#!/usr/bin/env python3
"""Mutation survivor check using the exhaustive libtny fault lane."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
subprocess.run(["make", "test-libtny-fault"], cwd=ROOT, check=True, timeout=600)
