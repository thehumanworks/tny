import subprocess
import sys
import tempfile
from pathlib import Path

w = Path(sys.argv[1])
assert len((w / "aggregates.c").read_text().splitlines()) >= 12000
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
