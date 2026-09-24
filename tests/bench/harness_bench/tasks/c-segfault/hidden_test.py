import subprocess
import sys
import tempfile
from pathlib import Path

w = Path(sys.argv[1])
with tempfile.TemporaryDirectory() as t:
    src = Path(t) / "check.c"
    src.write_text(
        '#include "tokenizer.h"\n#include <assert.h>\n#include <string.h>\nint main(void){const char *a[]={""," \\t\\n ","a","a  b\\tc"," odd  spaces "};size_t e[]={0,0,1,3,2};for(size_t i=0;i<5;++i){size_t n=999;char **v=split_words(a[i],&n);assert(v&&n==e[i]&&v[n]==NULL);free_words(v);}return 0;}\n'
    )
    exe = Path(t) / "check"
    probe = subprocess.run(
        ["cc", "-fsanitize=address", "-x", "c", "-", "-o", str(Path(t) / "probe")],
        input="int main(void){return 0;}\n",
        text=True,
        capture_output=True,
    )
    if probe.returncode != 0:
        raise RuntimeError("ASan compiler support is required for this task")
    runtime = subprocess.run([str(Path(t) / "probe")], capture_output=True, check=False)
    if runtime.returncode != 0:
        raise RuntimeError("ASan runtime is unavailable for this task")
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address"]
    subprocess.run(
        ["cc", *flags, "-I", str(w), str(w / "tokenizer.c"), str(src), "-o", str(exe)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        [str(exe)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
