import subprocess
import sys
import tempfile
from pathlib import Path

w = Path(sys.argv[1])
subprocess.run(
    ["make"], cwd=w, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
)
subprocess.run(
    ["make", "test"],
    cwd=w,
    check=True,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
with tempfile.TemporaryDirectory() as t:
    src = Path(t) / "check.c"
    src.write_text(
        '#include "mathutil.h"\n#include <assert.h>\nint main(void){int a[]={-3,5,9,0};assert(sum_values(a,0)==0);assert(sum_values(a,4)==11);return 0;}\n'
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
            str(src),
            str(w / "mathutil.c"),
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
