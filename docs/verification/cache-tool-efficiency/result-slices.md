# Stored-result slice verification

## Scope and evidence

The production `session_read_result` path now seeks and reads the requested byte range instead of slurping the complete result before slicing. The in-memory ephemeral branch is unchanged. The regression test creates a sparse 256 MiB + 4 byte stored result and checks initial binary bytes, a range crossing the sparse tail, a short EOF read, zero length, and `SIZE_MAX` offset. Both implementations passed all 19 assertions.

Baseline source: commit `f07acac68969675995764f6675c4803fb90ff6b0`, `src/core/session.c`. The baseline executable used the candidate test executable's exact other objects and link arguments, replacing only the session object with that original production source. This isolates the implementation change; it is not a reimplementation or a whole-repository baseline performance comparison.

One process per observation, ASan/UBSan debug build, `ASAN_OPTIONS=detect_leaks=0`:

| Implementation | Wall seconds | Peak child RSS, KiB |
| --- | ---: | ---: |
| Original session implementation | 1.8222 | 889160 |
| Bounded slice implementation | 0.0268 | 78852 |

The observations are single runs, not latency distributions. RSS is process-wide and includes the test harness and ASan. The sparse file is not representative of disk throughput. These numbers establish the eliminated full-buffer allocation/read work, not a general workload speedup. The timing measurement predates the additional address-space-budget assertion described below.

LeakSanitizer was disabled because running the same focused test with defaults produces `LeakSanitizer has encountered a fatal error` and `LeakSanitizer does not work under ptrace (strace, gdb, etc)` in this execution environment. AddressSanitizer and UndefinedBehaviorSanitizer remain enabled; this is not a passing leak gate.

## Reproduction

Build the debug test binary with the repository's pinned runtime. In this checkout, an already-built identical runtime was reused. Its ldlibs uses relative paths, so the ignored build symlink is also needed:

```sh
runtime=/path/to/prebuilt/pinned-runtime
ln -s "$runtime" build/cpython-3.14.7-x86_64-linux-gnu-4181434840
make -j4 debug CPYTHON_DIR="$runtime" -o "$runtime/libpython3.14.a" > /tmp/tny-tool-build.log 2>&1
```

The symlink name must match the relative runtime directory recorded in its `ldlibs`. For a clean environment, build the pinned runtime normally instead. `-o` is appropriate only when the existing runtime is already verified and matches the source pin. The captured log must contain the final test linker command; force relinking if necessary by touching a test source, then rebuilding.

The exact baseline object/relink operation used:

```python
import pathlib
import subprocess

log = pathlib.Path("/tmp/tny-tool-build.log").read_text()
link = [line for line in log.splitlines()
        if line.startswith("c++ ") and "-o build/tny-test " in line][-1]
source = subprocess.check_output([
    "git", "show", "f07acac68969675995764f6675c4803fb90ff6b0:src/core/session.c"
])
pathlib.Path("/tmp/tny-session-baseline.c").write_bytes(source)
subprocess.run(
    "cc -std=c11 -Iinclude -Isrc -Ithird_party -Ithird_party/yyjson "
    "-D_GNU_SOURCE -O0 -g -pthread -fsanitize=address,undefined "
    "-c /tmp/tny-session-baseline.c -o /tmp/tny-session-baseline.o",
    shell=True, check=True,
)
link = link.replace("-o build/tny-test ", "-o /tmp/tny-test-session-baseline ")
link = link.replace("build/dbg/src/core/session.o", "/tmp/tny-session-baseline.o")
subprocess.run(link, shell=True, check=True)
```

The baseline compilation explicitly uses the same optimization and sanitizer settings. It omits unrelated warning/dependency-generation flags from the normal object recipe. Run each measurement in a fresh Python process so `RUSAGE_CHILDREN.ru_maxrss` is not the cumulative maximum of previous children:

```sh
for binary in build/tny-test /tmp/tny-test-session-baseline; do
  ASAN_OPTIONS=detect_leaks=0 python3 - "$binary" <<'PYTHON'
import resource
import subprocess
import sys
import time
start = time.monotonic()
result = subprocess.run([
    sys.argv[1], "-s", "core_suite", "-t", "session_result_sparse_slices"
], capture_output=True, text=True)
print(result.stdout, result.stderr)
print({"binary": sys.argv[1], "returncode": result.returncode,
       "elapsed_seconds": round(time.monotonic() - start, 4),
       "maxrss_kb": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss})
PYTHON
done
```

## Artifact hashes

SHA-256 at measurement time (candidate source was formatted after its build; only whitespace changed):

- `build/tny-test`: `9507724574794dbf1744d1a1b438b1fc31b96af50d8b2f0053687ee67db36373`
- `/tmp/tny-test-session-baseline`: `c3853e222ee86ec99ff1cbcc21933c8498f001a4f27017935cf7006eb065adf8`
- `src/core/session.c`: `8fa6818fdc52862b7bd28e9614f0cf8733d398624db90b07bea271352c8e0538`
- `/tmp/tny-session-baseline.c`: `d1456eb3647c6f9acb89c19e130298ed2fc4f21f603ecb3e8d7fb761634777e4`

## MCP discovery output

Both empty-query and keyword discovery now use the existing configured preview-and-spill contract. That contract retains half the configured byte budget as content, then appends truncation metadata and an optional handle. It is **not** a strict bound on the total response bytes. Without a session there is no handle; ephemeral sessions retain spills in memory.

A one-off local fake MCP server with 1000 tools (120-byte descriptions) exercised the production search and spill paths: full discovery text was 141055 bytes; the default 32768-byte configured budget produced 16537 bytes including metadata. Both query modes recovered the complete original output through the handle, in disk and ephemeral sessions. This is output-size evidence, not a latency claim. The committed compact regression uses a reduced configured budget with the existing three-tool fixture to cover the same branches without enlarging the regular test fixture.

Focused ASan/UBSan results: session result tests 2/2 (27 assertions), MCP search tests 2/2 (43), ephemeral suite 4/4 (26), skills suite 9/9 (84), learning suite 13/13 (163). These are focused gates, not the full repository or leak gate.

## Mechanically enforced allocation bound

The final Linux regression also forks a child that reads its existing virtual size from `/proc/self/statm` and sets `RLIMIT_AS` to that size plus 64 MiB (clamped to any existing hard limit). Accounting for the existing virtual size preserves ASan's already-reserved shadow map. The child then requests four bytes at the tail of the 256 MiB sparse file and must exit successfully. The limit is isolated to the child; no production fault hooks or timing threshold are involved. Other platforms still run the byte-range assertions.

Rebuild/relink using the same commands above with the final test object, capturing the linker command in `/tmp/tny-tool-build-bound.log` and naming the baseline executable `/tmp/tny-test-session-bound-baseline`. Results:

- Candidate: both session-result tests pass, 31 assertions.
- Original session implementation: sparse test fails at the child exit assertion, with `ERROR: Failed to mmap`; exit status 1. This is the expected regression rejection, not a passing test.

Thus the old implementation is caught by an allocation constraint, independently of elapsed-time or RSS measurements. The maximum allocation headroom is a test invariant, not a claim about all runtime allocations.

Final constrained-test executable SHA-256 values:
- `build/tny-test`: `79d03ffae5af1471066a12e07bf9c81dad90ba7528e55ddc95bb30197ff7c5b8`
- `/tmp/tny-test-session-bound-baseline`: `4c4816bcc3c20f0488509f05a8ed061315bcadbbcc084eb0235882ae23ea722f`
