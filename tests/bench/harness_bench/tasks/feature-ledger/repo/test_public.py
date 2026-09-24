import tempfile
from pathlib import Path

from ledger import Ledger

with tempfile.TemporaryDirectory() as d:
    with Ledger(Path(d) / "ledger.db") as x:
        assert x.apply("e1", "apple", 3) == 3
        assert x.apply("e1", "apple", 3) == 3
        assert x.snapshot() == {"apple": 3}
