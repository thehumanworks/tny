#!/usr/bin/env python3
"""Build and compare an identical C-facing parser corpus against two source trees.

Both builds use their Makefile's native release flags and the existing tny
allocator instrumentation. Vendored sources retain the repository exemption;
first-party code compiles warning-free. This microbenchmark measures parser
work, not CLI startup. Reports include all build commands, compiler identities,
source/artifact hashes, output checksums, allocation counts and peak RSS.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shlex
import statistics
import subprocess
from pathlib import Path
from typing import Any

MODES = ("sse", "connect", "tools")
PATTERNS = ("whole", "byte", "split")
SOURCE_STEMS = (
    "src/util/util",
    "src/util/alloc",
    "src/json/json",
    "src/net/sse",
    "src/net/connectrpc",
    "src/backends/openai/toolcalls",
    "third_party/yyjson/yyjson",
)
CONFIG_MAKEFILE = """
.PHONY: tny-parser-benchmark-config
tny-parser-benchmark-config:
\t@printf '%s\\n' 'CC=$(CC)' 'CXX=$(CXX)' \\
\t 'CFLAGS=$(REL_CFLAGS) $(REL_LTO) $(REL_INLINE) $(REL_SIZE_OPT)' \\
\t 'CXXFLAGS=$(REL_CXXFLAGS) $(REL_LTO) $(REL_INLINE) $(REL_SIZE_OPT)' \\
\t 'HAS_CXXFLAGS=$(strip $(REL_CXXFLAGS))' 'LDFLAGS=$(REL_LDFLAGS)'
"""


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_file(root: Path, stem: str) -> Path:
    matches = [root / (stem + suffix) for suffix in (".c", ".cpp")]
    matches = [path for path in matches if path.is_file()]
    if len(matches) != 1:
        raise ValueError(f"expected exactly one C or C++ source for {stem}: {matches}")
    return matches[0]


def checked(command: list[str], cwd: Path, env: dict[str, str], log: Path) -> str:
    result = subprocess.run(
        command, cwd=cwd, env=env, text=True, capture_output=True, check=False
    )
    with log.open("a", encoding="utf-8") as output:
        output.write(
            f"$ {shlex.join(command)}\n{result.stdout}{result.stderr}\nexit={result.returncode}\n"
        )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed with status {result.returncode}; see {log}: {shlex.join(command)}"
        )
    if result.stderr:
        raise RuntimeError(
            f"unexpected diagnostic output; see {log}: {shlex.join(command)}"
        )
    return result.stdout


def manifest(root: Path, sources: list[Path], harness: Path) -> dict[str, str]:
    paths = set(sources)
    for directory in ("src", "include", "third_party/yyjson", "third_party/wslay"):
        paths.update(
            path
            for path in (root / directory).rglob("*")
            if path.suffix in (".h", ".hpp")
        )
    result = {str(path.relative_to(root)): digest(path) for path in sorted(paths)}
    result["Makefile"] = digest(root / "Makefile")
    result["<shared-benchmark-harness>"] = digest(harness)
    return result


def build(
    root: Path, out: Path, harness: Path, args: argparse.Namespace, env: dict[str, str]
) -> dict[str, Any]:
    out.mkdir(parents=True, exist_ok=False)
    log = out / "build.log"
    extra = out / "benchmark.mk"
    extra.write_text(CONFIG_MAKEFILE, encoding="utf-8")
    command = [
        args.make,
        "--no-print-directory",
        "-s",
        "-f",
        "Makefile",
        "-f",
        str(extra),
        f"CC={args.cc}",
        f"CXX={args.cxx}",
        f"BUILD={out / 'generated-build'}",
        "tny-parser-benchmark-config",
    ]
    values = checked(command, root, env, log)
    config = dict(line.split("=", 1) for line in values.splitlines())
    cflags = shlex.split(config["CFLAGS"])
    cppflags = shlex.split(config["CXXFLAGS"])
    if not config["HAS_CXXFLAGS"]:
        cppflags = [flag for flag in cflags if not flag.startswith("-std=")]
        cppflags.extend(("-std=c++20", "-fno-rtti"))
    sources = [source_file(root, stem) for stem in SOURCE_STEMS]
    before = manifest(root, sources, harness)
    commands: list[list[str]] = []
    objects: list[str] = []
    for index, source in enumerate([*sources, harness]):
        cpp = source.suffix == ".cpp"
        flags = list(cppflags if cpp else cflags)
        flags.extend(("-DTNY_ALLOC_TESTING=1", "-DTNY_SHARED_LIBRARY_BUILD=1"))
        vendored = source.is_relative_to(root / "third_party")
        if vendored:
            flags.append("-w")
        elif not cpp and source != root / "src/util/alloc.c":
            flags.extend(("-include", "src/util/alloc_override.h"))
        obj = out / f"{index}.o"
        command = shlex.split(config["CXX"] if cpp else config["CC"])
        command += [*flags, "-c", str(source), "-o", str(obj)]
        checked(command, root, env, log)
        commands.append(command)
        objects.append(str(obj))
    binary = out / "bench-parsers"
    # The pre-series C baseline must not gain an artificial C++ runtime load.
    link = (
        config["CXX"]
        if any(source.suffix == ".cpp" for source in sources)
        else config["CC"]
    )
    command = [
        *shlex.split(link),
        *objects,
        *shlex.split(config["LDFLAGS"]),
        "-o",
        str(binary),
    ]
    checked(command, root, env, log)
    commands.append(command)
    if manifest(root, sources, harness) != before:
        raise RuntimeError("source changed during benchmark compilation")
    compiler_versions = {
        name: checked([*shlex.split(config[name]), "--version"], root, env, log)
        for name in ("CC", "CXX")
    }
    dependency_command = (
        ["otool", "-L", str(binary)]
        if platform.system() == "Darwin"
        else ["ldd", str(binary)]
    )
    dependencies = subprocess.run(
        dependency_command, text=True, capture_output=True, check=False
    )
    return {
        "root": str(root),
        "binary": str(binary),
        "sha256": digest(binary),
        "bytes": binary.stat().st_size,
        "sources": before,
        "configuration": config,
        "commands": commands,
        "compiler_versions": compiler_versions,
        "dependencies": {
            "command": dependency_command,
            "status": dependencies.returncode,
            "stdout": dependencies.stdout,
            "stderr": dependencies.stderr,
        },
        "build_log": str(log),
    }


def compare(
    baseline: list[dict[str, Any]], candidate: list[dict[str, Any]]
) -> dict[str, Any]:
    if not baseline or len(baseline) != len(candidate):
        raise ValueError("comparison requires equally sized, nonempty sample sets")
    identities = (
        "mode",
        "fragmentation",
        "iterations",
        "input_bytes",
        "events",
        "checksum",
    )
    expected = tuple(baseline[0][field] for field in identities)
    for sample in baseline + candidate:
        if tuple(sample[field] for field in identities) != expected:
            raise ValueError("parser output/corpus oracle differs between runs")
        if any(
            not isinstance(sample[key], int) or sample[key] <= 0
            for key in (
                "nanoseconds",
                "peak_rss_bytes",
                "iterations",
                "input_bytes",
                "events",
            )
        ):
            raise ValueError("invalid parser measurements")
        if not isinstance(sample["allocations"], int) or sample["allocations"] < 0:
            raise ValueError("invalid allocation count")
    result: dict[str, Any] = {"identity": dict(zip(identities, expected, strict=True))}
    for label, samples in (("baseline", baseline), ("candidate", candidate)):
        result[label] = {
            "median_nanoseconds": statistics.median(
                sample["nanoseconds"] for sample in samples
            ),
            "median_peak_rss_bytes": statistics.median(
                sample["peak_rss_bytes"] for sample in samples
            ),
            "allocations": [sample["allocations"] for sample in samples],
        }
    result["time_ratio"] = (
        result["candidate"]["median_nanoseconds"]
        / result["baseline"]["median_nanoseconds"]
    )
    result["rss_ratio"] = (
        result["candidate"]["median_peak_rss_bytes"]
        / result["baseline"]["median_peak_rss_bytes"]
    )
    result["maximum_ratio"] = 1.10
    result["passed"] = result["time_ratio"] <= 1.10 and result["rss_ratio"] <= 1.10
    return result


def execute(args: argparse.Namespace) -> dict[str, Any]:
    root = args.work_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    for key in ("TNY_TOOLS", "TNY_TEST_ALLOC_SCOPE", "TNY_TEST_ALLOC_FAIL_AT"):
        env.pop(key, None)
    harness = Path(__file__).with_suffix(".c").resolve()
    report: dict[str, Any] = {
        "schema": 1,
        "host": platform.platform(),
        "iterations": args.iterations,
        "allocation_instrumentation": "existing tny allocator on both artifacts, including yyjson",
        "batches": 3,
        "builds": {},
        "samples": {},
        "comparisons": {},
        "passed": False,
    }
    try:
        for name in ("baseline", "candidate"):
            report["builds"][name] = build(
                getattr(args, name).resolve(), root / name, harness, args, env
            )
        for mode in MODES:
            for pattern in PATTERNS:
                key = f"{mode}/{pattern}"
                values: dict[str, list[dict[str, Any]]] = {
                    "baseline": [],
                    "candidate": [],
                }
                report["samples"][key] = values
                for batch in range(3):
                    order = (
                        ("baseline", "candidate")
                        if batch % 2 == 0
                        else ("candidate", "baseline")
                    )
                    for name in order:
                        command = [
                            report["builds"][name]["binary"],
                            mode,
                            pattern,
                            str(args.iterations),
                        ]
                        sample = json.loads(
                            checked(command, root, env, root / "runs.log")
                        )
                        sample["batch"] = batch
                        values[name].append(sample)
                report["comparisons"][key] = compare(
                    values["baseline"], values["candidate"]
                )
        for metadata in report["builds"].values():
            if digest(Path(metadata["binary"])) != metadata["sha256"]:
                raise RuntimeError("benchmark artifact changed during measurement")
        report["passed"] = all(
            value["passed"] for value in report["comparisons"].values()
        )
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
    (root / "report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument(
        "--work-dir", type=Path, required=True, help="new, empty output directory"
    )
    parser.add_argument("--iterations", type=int, default=2000)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--make", default="make")
    args = parser.parse_args()
    if not 1 <= args.iterations <= 1000000:
        parser.error("--iterations must be between 1 and 1000000")
    try:
        report = execute(args)
    except OSError as exc:
        parser.error(str(exc))
    for name, value in report["comparisons"].items():
        print(
            f"{name}: time={value['time_ratio']:.3f}x rss={value['rss_ratio']:.3f}x "
            f"{'PASS' if value['passed'] else 'FAIL'}"
        )
    if "error" in report:
        print(f"benchmark failed: {report['error']}")
    print(f"report: {args.work_dir / 'report.json'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
