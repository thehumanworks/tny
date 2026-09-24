import ctypes
import json
import re
import sys
from pathlib import Path

w = Path(sys.argv[1])
actual = json.loads((w / "ANSWERS.json").read_text())
assert isinstance(actual, dict) and set(actual) == {f"q{i}" for i in range(1, 6)}


def single_integer(value):
    numbers = re.findall(r"(?<!\w)-?\d+(?!\w)", str(value))
    assert len(numbers) == 1, value
    return int(numbers[0])


assert single_integer(actual["q1"]) == -2
assert single_integer(actual["q2"]) == 3
assert single_integer(actual["q3"]) == 99999999
expression = re.sub(r"\s+", "", str(actual["q4"])).lower()
assert expression in {
    "sizeof(size_t)*2",
    "2*sizeof(size_t)",
    str(ctypes.sizeof(ctypes.c_size_t) * 2),
}
field = str(actual["q5"]).strip().strip("`")
assert re.fullmatch(r"(?:[A-Za-z_]\w*(?:->|\.))?consume_trailer", field)
assert "Copyright" in (w / "LICENSE").read_text()
