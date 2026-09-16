#!/usr/bin/env python3
"""Offline spawn-to-exit / PTY prompt benchmarks; Python 3.14, stdlib only."""

import argparse
import fcntl
import hashlib
import json
import math
import os
import platform
import pty
import resource
import select
import shutil
import signal
import statistics
import struct
import subprocess
import sys
import tempfile
import termios
import time
from decimal import Decimal
from pathlib import Path

PROMPT = b"\x1b[1m\x1b[32m> \x1b[0m"
POLICY = "0115-v1"
MODES = ("help", "version", "prompt")


def summary(samples):
    values = sorted(samples)
    return {
        "median": statistics.median(values),
        "p95": values[math.ceil(len(values) * 0.95) - 1],
        "raw": samples,
    }


def gates(baseline, candidate):
    result = {}
    for mode in MODES:
        before = baseline[mode]["latency_ms"]["median"]
        after = candidate[mode]["latency_ms"]["median"]
        ceiling, floor = (10, 0.5) if mode == "prompt" else (5, 0.25)
        before_decimal = Decimal(str(before))
        after_decimal = Decimal(str(after))
        delta = after_decimal - before_decimal
        allowance = max(Decimal(str(floor)), before_decimal * Decimal("0.1"))
        result[mode] = {
            "baseline_ms": before,
            "candidate_ms": after,
            "delta_ms": float(delta),
            "allowance_ms": float(allowance),
            "ceiling_ms": ceiling,
            "pass": after_decimal < ceiling and delta <= allowance,
        }
    return result


def launch(binary, mode, home, timeout=5):
    # Allowlist: no inherited credentials, config, preload, or provider settings.
    env = {
        "HOME": home,
        "XDG_CONFIG_HOME": home,
        "XDG_CACHE_HOME": home,
        "XDG_DATA_HOME": home,
        "PATH": "/usr/bin:/bin",
        "TERM": "xterm-256color",
        "LANG": "C",
        "LC_ALL": "C",
    }
    master = slave = None
    proc = None
    try:
        args = [str(binary), "--" + mode]
        if mode == "prompt":
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
            args = [str(binary), "--provider", "openai", "--cwd", home]
        start = time.perf_counter_ns()
        proc = subprocess.Popen(
            args,
            cwd=home,
            env=env,
            start_new_session=True,
            stdin=slave if slave is not None else subprocess.DEVNULL,
            stdout=slave if slave is not None else subprocess.PIPE,
            stderr=slave if slave is not None else subprocess.PIPE,
        )
        if slave is not None:
            os.close(slave)
            slave = None
            data = b""
            deadline = time.monotonic() + timeout
            while PROMPT not in data:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError("first prompt timed out")
                if select.select([master], [], [], remaining)[0]:
                    chunk = os.read(master, 65536)
                    if not chunk:
                        raise RuntimeError("PTY closed before prompt")
                    data = (data + chunk)[-131072:]
            elapsed = (time.perf_counter_ns() - start) / 1e6
        else:
            proc.communicate(timeout=timeout)
            elapsed = (time.perf_counter_ns() - start) / 1e6
            if proc.returncode:
                raise RuntimeError(f"{mode} exited {proc.returncode}")
    finally:
        if proc is not None:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        for fd in (master, slave):
            if fd is not None:
                os.close(fd)
    # Called in a fresh measuring worker: RUSAGE_CHILDREN has exactly one child.
    rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    return {"ms": elapsed, "rss_bytes": rss if sys.platform == "darwin" else rss * 1024}


def measure(binary, mode, timeout=5):
    # Fork outside the timed interval to reset the cumulative children RSS peak.
    with tempfile.TemporaryDirectory(prefix="tny-startup-") as home:
        read_fd, write_fd = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(read_fd)
            try:
                result = launch(binary, mode, home, timeout)
            except Exception as exc:
                result = {"error": str(exc)}
            with os.fdopen(write_fd, "w") as stream:
                json.dump(result, stream)
            os._exit(0)
        os.close(write_fd)
        with os.fdopen(read_fd) as stream:
            result = json.load(stream)
        os.waitpid(pid, 0)
        if "error" in result:
            raise RuntimeError(result["error"])
        return result


def size_report(binary, wasm_dir=None):
    binary = Path(binary).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="tny-size-") as tmp:
        copy = Path(tmp) / binary.name
        shutil.copy2(binary, copy)
        subprocess.run(["strip", str(copy)], check=True, capture_output=True)
        stripped = copy.stat().st_size
    command = ["otool", "-L"] if sys.platform == "darwin" else ["ldd"]
    deps = subprocess.run([*command, str(binary)], capture_output=True, text=True)
    output = deps.stdout + deps.stderr
    static = "not a dynamic executable" in output or "statically linked" in output
    if deps.returncode and not static:
        raise RuntimeError(f"dependency inspection failed: {output.strip()}")
    directory = Path(wasm_dir) if wasm_dir else binary.parent / "wasm"
    artifacts = {
        p.name: p.stat().st_size
        for p in sorted(directory.glob("*"))
        if p.is_file() and p.suffix in (".wasm", ".js", ".mjs")
    }
    return {
        "path": str(binary),
        "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "file_bytes": binary.stat().st_size,
        "stripped_bytes": stripped,
        "dependency_command": command,
        "dependencies": output.strip(),
        "cpp_runtime_dependencies": [
            line.strip()
            for line in output.splitlines()
            if "libc++" in line or "libstdc++" in line
        ],
        "wasm": {
            "directory": str(directory),
            "available": bool(artifacts),
            "artifacts_bytes": artifacts,
            "total_bytes": sum(artifacts.values()),
        },
    }


def markdown(report, *, informational=False):
    lines = [
        f"# Startup: {report['label']}",
        "",
        f"Policy: {POLICY}; host: {report['host']}",
        f"Build metadata: {report.get('build_metadata', 'unspecified')}",
        "",
        "| Metric | Baseline median ms | Candidate median ms | Delta ms | Result |",
        "| --- | ---: | ---: | ---: | --- |",
    ]
    if informational:
        lines.insert(
            2, "Cross-report comparison: informational, not a contract verdict.\n"
        )
    for mode, gate in report["gates"].items():
        verdict = (
            "informational" if informational else ("PASS" if gate["pass"] else "FAIL")
        )
        lines.append(
            f"| {mode} | {gate['baseline_ms']:.4f} | {gate['candidate_ms']:.4f} | "
            f"{gate['delta_ms']:+.4f} | {verdict} |"
        )
    for role in ("baseline", "candidate"):
        item = report[role]
        lines += [
            "",
            f"## {role}",
            "",
            f"Stripped bytes: {item['size']['stripped_bytes']}",
            f"C++ runtimes: {item['size']['cpp_runtime_dependencies'] or 'none'}",
            "",
            "```text",
            item["size"]["dependencies"],
            "```",
            "",
            "| Metric | p95 ms | Peak child RSS bytes |",
            "| --- | ---: | ---: |",
        ]
        for mode in MODES:
            lines.append(
                f"| {mode} | {item[mode]['latency_ms']['p95']:.4f} | "
                f"{max(item[mode]['rss_bytes']['raw'])} |"
            )
        lines += [
            "",
            f"Wasm accounting: `{json.dumps(item['size']['wasm'], sort_keys=True)}`",
        ]
    return "\n".join(lines) + "\n"


def benchmark(args):
    report = {
        "schema": 1,
        "policy": POLICY,
        "label": args.label,
        "host": platform.platform(),
        "python": platform.python_version(),
        "build_metadata": args.build_metadata,
        "order": [],
        "method": {
            "batches": 3,
            "cli_samples": args.samples,
            "prompt_samples": args.prompt_samples,
            "prompt_hex": PROMPT.hex(),
            "rss": "fresh worker RUSAGE_CHILDREN ru_maxrss; peak through termination",
        },
    }
    binaries = {
        role: Path(getattr(args, role)).resolve(strict=True)
        for role in ("baseline", "candidate")
    }
    for role, binary in binaries.items():
        report[role] = {"size": size_report(binary, args.wasm_dir)}
    for mode in MODES:
        samples = {role: [] for role in binaries}
        count = args.prompt_samples if mode == "prompt" else args.samples
        for batch in range(3):
            roles = (
                ("baseline", "candidate")
                if batch % 2 == 0
                else ("candidate", "baseline")
            )
            for _ in range(count // 3 + (batch < count % 3)):
                for role in roles:
                    samples[role].append(measure(binaries[role], mode))
                    report["order"].append([mode, batch + 1, role, len(samples[role])])
        for role in binaries:
            report[role][mode] = {
                "latency_ms": summary([v["ms"] for v in samples[role]]),
                "rss_bytes": summary([v["rss_bytes"] for v in samples[role]]),
            }
    report["gates"] = gates(report["baseline"], report["candidate"])
    return report


def validate_report(report):
    if report.get("policy") != POLICY or report.get("schema") != 1:
        raise ValueError("incompatible reporting policy/schema")
    for mode in MODES:
        item = report["candidate"][mode]
        raw = item["latency_ms"]["raw"]
        minimum = 20 if mode == "prompt" else 100
        if len(raw) < minimum or any(not math.isfinite(v) or v < 0 for v in raw):
            raise ValueError(f"invalid {mode} samples")
        if item["latency_ms"] != summary(raw):
            raise ValueError(f"inconsistent {mode} statistics")
        if len(item["rss_bytes"]["raw"]) != len(raw):
            raise ValueError(f"missing {mode} RSS observations")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline")
    parser.add_argument("--candidate")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--label", default="startup")
    parser.add_argument("--samples", type=int, default=102)
    parser.add_argument("--prompt-samples", type=int, default=21)
    parser.add_argument(
        "--build-metadata", default="unspecified; record compiler and release flags"
    )
    parser.add_argument("--wasm-dir", type=Path)
    parser.add_argument("--size-only", action="store_true")
    parser.add_argument(
        "--compare",
        nargs=2,
        type=Path,
        metavar=("BEFORE", "AFTER"),
        help="informational cross-report deltas, not a contract verdict",
    )
    args = parser.parse_args()
    try:
        if args.compare:
            before, after = [json.loads(p.read_text()) for p in args.compare]
            for result in (before, after):
                validate_report(result)
            if before["host"] != after["host"]:
                raise ValueError("comparison requires the same host platform")
            report = dict(after, baseline=before["candidate"], label=args.label)
            report["gates"] = gates(report["baseline"], report["candidate"])
            print(markdown(report, informational=True), end="")
            print(
                "Stripped size delta:",
                report["candidate"]["size"]["stripped_bytes"]
                - report["baseline"]["size"]["stripped_bytes"],
                "bytes",
            )
            return 0
        elif args.size_only:
            if not args.candidate:
                parser.error("--size-only requires --candidate")
            print(
                json.dumps(
                    size_report(args.candidate, args.wasm_dir), indent=2, sort_keys=True
                )
            )
            return 0
        else:
            if not args.baseline or not args.candidate or not args.json:
                parser.error("benchmark requires --baseline, --candidate, --json")
            if args.samples < 100 or args.prompt_samples < 20:
                parser.error("requires >=100 CLI and >=20 prompt samples per binary")
            report = benchmark(args)
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
            args.json.with_suffix(".md").write_text(markdown(report))
            print(markdown(report), end="")
        return 0 if all(g["pass"] for g in report["gates"].values()) else 1
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        RuntimeError,
        subprocess.SubprocessError,
    ) as exc:
        print(f"benchmark error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
