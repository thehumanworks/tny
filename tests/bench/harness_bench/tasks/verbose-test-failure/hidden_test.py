import subprocess
import sys
from pathlib import Path

w = Path(sys.argv[1])
sys.path.insert(0, str(w))
from parcelcalc.money import parse_cents

for v, n in [
    ("0", 0),
    ("+1.29", 129),
    ("-0.01", -1),
    ("999999999999999.99", 99999999999999999),
    ("1.2", 120),
    (".05", 5),
    ("2.", 200),
]:
    assert parse_cents(v) == n
for v in ["", " 1", "1 ", "1.234", "1e2", "NaN", "--1", None, True, 12]:
    try:
        parse_cents(v)
    except ValueError:
        pass
    else:
        raise AssertionError(v)
subprocess.run(
    [sys.executable, "-m", "unittest", "discover", "-s", "tests"],
    cwd=w,
    check=True,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
