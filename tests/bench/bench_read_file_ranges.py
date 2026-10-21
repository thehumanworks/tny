#!/usr/bin/env python3
"""Compare real read_file owners with identical builds, without model inference.

Counts bytes returned by fread, not physical disk I/O. Fresh child peak RSS
includes process overhead and the inherited fork/exec high-water floor; latency
covers the file tool only, not a full agent.
Sparse zero tails isolate range cost; results are identical ASCII first 20 lines.
Page cache is uncontrolled, so samples do not represent cold-storage latency.
"""

import argparse
import hashlib
import json
import os
import platform
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def checked(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default="fe02b5cb")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sizes-mib", nargs="+", type=int, default=[1, 100, 1000])
    parser.add_argument("--samples", type=int, default=3)
    args = parser.parse_args()
    compiler = os.environ.get("CC", "cc")
    flags = [
        "-Os",
        "-std=c11",
        "-D_GNU_SOURCE",
        "-ffunction-sections",
        "-fdata-sections",
    ]
    includes = ["-Iinclude", "-Isrc", "-Ithird_party/yyjson"]
    strip = "-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections"
    sources = [
        "src/util/util.c",
        "src/util/alloc.c",
        "src/json/json.c",
        "src/core/image.c",
        "third_party/yyjson/yyjson.c",
    ]
    report = {
        "scope": __doc__,
        "platform": platform.platform(),
        "compiler": checked([compiler, "--version"]).stdout.decode().splitlines()[0],
        "flags": flags,
        "baseline_revision": args.baseline,
        "shared_source_sha256": {
            p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest()
            for p in sources + ["tests/bench/bench_read_file_ranges.c"]
        },
    }
    prefix = b"".join(f"line {i:02d}: benchmark range\n".encode() for i in range(1, 21))
    with tempfile.TemporaryDirectory(prefix="tny-read-range-bench-") as temporary:
        work = Path(temporary)
        objects = []
        for i, source in enumerate(sources):
            obj = work / f"shared-{i}.o"
            instrumentation = (
                ["-Dfread=measured_fread"] if source.endswith("util.c") else []
            )
            checked(
                [
                    compiler,
                    *flags,
                    *includes,
                    *instrumentation,
                    "-c",
                    source,
                    "-o",
                    str(obj),
                ],
                cwd=ROOT,
            )
            objects.append(str(obj))
        variants = {
            "baseline": checked(
                ["git", "show", f"{args.baseline}:src/core/tools_fs.c"], cwd=ROOT
            ).stdout,
            "candidate": (ROOT / "src/core/tools_fs.c").read_bytes(),
        }
        for name, source in variants.items():
            path = work / f"{name}.c"
            path.write_bytes(source)
            binary = work / name
            checked(
                [
                    compiler,
                    *flags,
                    *includes,
                    f'-DTNY_FS_SOURCE="{path}"',
                    "tests/bench/bench_read_file_ranges.c",
                    *objects,
                    strip,
                    "-pthread",
                    "-lm",
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
            )
            report[name] = {
                "owner_sha256": hashlib.sha256(source).hexdigest(),
                "samples": [],
            }
        for size in args.sizes_mib:
            corpus = work / "corpus"
            with corpus.open("wb") as file:
                file.write(prefix)
                file.truncate(size * 1024 * 1024)
            for _ in range(args.samples):
                for name in variants:
                    result = checked([str(work / name), str(corpus)])
                    if result.stdout != prefix:
                        raise AssertionError(
                            f"{name} output differs from exact expected prefix"
                        )
                    sample = json.loads(result.stderr)
                    sample.update(
                        size_mib=size,
                        output_sha256=hashlib.sha256(result.stdout).hexdigest(),
                    )
                    report[name]["samples"].append(sample)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
