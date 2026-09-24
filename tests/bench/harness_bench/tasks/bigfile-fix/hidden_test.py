import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

w = Path(sys.argv[1])
source = (w / "aggregates.c").read_bytes()
assert len(source.splitlines()) >= 12000
assert source.count(b"long scan_217(") == 1
assert source.count(b"long scan_218(") == 1
start = source.index(b"long scan_217(")
end = source.index(b"long scan_218(", start)
assert hashlib.sha256(source[:start]).hexdigest() == (
    "1eff21987df9abe78b7affefe8b3215c2759145d8575df9a022be14095ef7cb4"
), "a function before scan_217 changed"
assert hashlib.sha256(source[end:]).hexdigest() == (
    "5d70afb8bf03fe7896c48a453f47d374477d23ca9ff04505c3c29d7ed983f9f5"
), "a function after scan_217 changed"
assert hashlib.sha256((w / "aggregates.h").read_bytes()).hexdigest() == (
    "3dbf5d5da70259810d7cbaf14c8847672161bbaa5fc76486ad7819e17a20a442"
), "generated declarations changed"
with tempfile.TemporaryDirectory() as t:
    src = Path(t) / "check.c"
    src.write_text(
        '#include "aggregates.h"\n#include <assert.h>\nint main(void){int a[]={-7,4,9,2};assert(scan_217(a,0)==0);assert(scan_217(a,1)==-7);assert(scan_217(a,4)==8);return 0;}\n'
    )
    exe = Path(t) / "check"
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(w),
            str(w / "aggregates.c"),
            str(src),
            "-o",
            str(exe),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        [str(exe)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
subprocess.run(
    ["make", "test"],
    cwd=w,
    check=True,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
