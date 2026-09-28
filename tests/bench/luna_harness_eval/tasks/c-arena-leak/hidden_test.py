import subprocess
import sys
import tempfile
from pathlib import Path

w = Path(sys.argv[1])
subprocess.run(
    ["make", "test"],
    cwd=w,
    check=True,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
source = r"""#include "arena.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
int main(void) {
    arena a = {0};
    for (int round = 0; round < 8; ++round) {
        for (size_t n = 0; n < 1000; n += 17) {
            unsigned char *p = arena_alloc(&a, n);
            assert(p && (uintptr_t)p % _Alignof(max_align_t) == 0);
            memset(p, 0x5a, n ? n : 1);
        }
        arena_reset(&a);
        assert(arena_live_chunks() == 0);
    }
    assert(arena_alloc(&a, 0));
    arena_destroy(&a);
    assert(arena_live_chunks() == 0);
    assert(arena_alloc(&a, SIZE_MAX) == NULL);
    arena_destroy(&a);
    return 0;
}
"""
with tempfile.TemporaryDirectory() as tmp:
    check = Path(tmp) / "check.c"
    check.write_text(source)
    binary = Path(tmp) / "check"
    probe = subprocess.run(
        ["cc", "-fsanitize=address", "-x", "c", "-", "-o", str(Path(tmp) / "probe")],
        input="int main(void){return 0;}\n",
        text=True,
        capture_output=True,
    )
    if probe.returncode != 0:
        raise RuntimeError("ASan compiler support is required for this task")
    runtime = subprocess.run(
        [str(Path(tmp) / "probe")], capture_output=True, check=False
    )
    if runtime.returncode != 0:
        raise RuntimeError("ASan runtime is unavailable for this task")
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address"]
    subprocess.run(
        ["cc", *flags, "-I", str(w), str(w / "arena.c"), str(check), "-o", str(binary)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        [str(binary)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
