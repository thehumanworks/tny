#!/usr/bin/env python3
"""Reproducible latency, durability, fairness, and size benchmarks for tnytty.

All runtime/config/socket paths are created under a private temporary root.
The optional baseline binary is measured only on surfaces it already has.
JSON is written to stdout and, when --output is supplied, atomically copied
to that path as well.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import select
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

HELP_VERSION_RUNS = 100
DEFAULT_RUNS = 50
DEFAULT_BROKER_RUNS = 20
READY_TIMEOUT = 5.0
REQUEST_TIMEOUT = 5.0


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def distribution(values: list[float]) -> dict[str, Any]:
    return {
        "unit": "ms",
        "n": len(values),
        "mean": statistics.fmean(values),
        "stdev": statistics.pstdev(values),
        "min": min(values),
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "max": max(values),
    }


def measured(count: int, operation: Callable[[], None]) -> dict[str, Any]:
    samples = []
    for _ in range(count):
        started = time.perf_counter_ns()
        operation()
        samples.append((time.perf_counter_ns() - started) / 1_000_000)
    return distribution(samples)


def isolated_env(root: Path) -> dict[str, str]:
    env = os.environ.copy()
    home = root / "home"
    config = root / "config"
    runtime = root / "runtime"
    for path in (home, config, runtime):
        path.mkdir(mode=0o700, parents=True, exist_ok=True)
        path.chmod(0o700)
    env.update(
        {
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(config),
            "XDG_RUNTIME_DIR": str(runtime),
        }
    )
    return env


def command_metric(binary: Path, flag: str, env: dict[str, str]) -> dict[str, Any]:
    command = [str(binary), flag]
    devnull = os.open(os.devnull, os.O_RDWR)
    actions = [
        (os.POSIX_SPAWN_DUP2, devnull, 0),
        (os.POSIX_SPAWN_DUP2, devnull, 1),
        (os.POSIX_SPAWN_DUP2, devnull, 2),
    ]

    def run() -> None:
        pid = os.posix_spawn(command[0], command, env, file_actions=actions)
        _, status = os.waitpid(pid, 0)
        if status != 0:
            raise RuntimeError(f"{' '.join(command)} exited with status {status}")

    try:
        for _ in range(20):
            run()
        return measured(HELP_VERSION_RUNS, run)
    finally:
        os.close(devnull)


def pty_sample(binary: Path, env: dict[str, str]) -> tuple[float, float]:
    command = [str(binary), "run", "--", "/bin/sh", "-c", "printf READY"]
    started = time.perf_counter_ns()
    proc = subprocess.Popen(
        command,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    assert proc.stdout is not None
    first = proc.stdout.read(1)
    first_ms = (time.perf_counter_ns() - started) / 1_000_000
    proc.communicate(timeout=REQUEST_TIMEOUT)
    full_ms = (time.perf_counter_ns() - started) / 1_000_000
    if first != b"R" or proc.returncode != 0:
        raise RuntimeError("PTY readiness command did not produce READY cleanly")
    return first_ms, full_ms


def stripped_size(binary: Path, root: Path) -> int:
    target = root / "tnytty.stripped"
    shutil.copy2(binary, target)
    strip = shutil.which("strip")
    if strip:
        subprocess.run([strip, str(target)], check=True, stdout=subprocess.DEVNULL)
    return target.stat().st_size


def binary_metrics(
    binary: Path, env: dict[str, str], root: Path, runs: int
) -> dict[str, Any]:
    root.mkdir(parents=True, exist_ok=True)
    first_samples = []
    full_samples = []
    for _ in range(10):
        pty_sample(binary, env)
    for _ in range(runs):
        first, full = pty_sample(binary, env)
        first_samples.append(first)
        full_samples.append(full)
    return {
        "path": str(binary),
        "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "stripped_bytes": stripped_size(binary, root),
        "help": command_metric(binary, "--help", env),
        "version": command_metric(binary, "--version", env),
        "pty_first_byte": distribution(first_samples),
        "full_process": distribution(full_samples),
    }


class Broker:
    def __init__(self, helper: Path, root: Path, env: dict[str, str]) -> None:
        self.socket_path = root / "broker" / "daemon.sock"
        self.socket_path.parent.mkdir(mode=0o700, parents=True)
        self.socket_path.parent.chmod(0o700)
        self.started_ns = time.perf_counter_ns()
        self.proc = subprocess.Popen(
            [str(helper), str(self.socket_path)],
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        assert self.proc.stdout is not None
        readable, _, _ = select.select([self.proc.stdout], [], [], READY_TIMEOUT)
        ready = self.proc.stdout.read(1) if readable else b""
        self.ready_ms = (time.perf_counter_ns() - self.started_ns) / 1_000_000
        if ready != b"1":
            self.stop()
            raise RuntimeError("benchmark broker did not become ready")

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=READY_TIMEOUT)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=READY_TIMEOUT)
        if self.proc.stdout:
            self.proc.stdout.close()

    def request(self, method: str, path: str, body: bytes = b"") -> tuple[int, bytes]:
        request = (
            f"{method} {path} HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: close\r\n\r\n"
        ).encode("ascii") + body
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(REQUEST_TIMEOUT)
            client.connect(str(self.socket_path))
            client.sendall(request)
            chunks = []
            while True:
                chunk = client.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
        raw = b"".join(chunks)
        head, separator, response = raw.partition(b"\r\n\r\n")
        if not separator:
            raise RuntimeError("broker returned a response without HTTP headers")
        try:
            status = int(head.split(b" ", 2)[1])
        except (IndexError, ValueError) as exc:
            raise RuntimeError("broker returned an invalid HTTP status") from exc
        return status, response

    def ok(self, method: str, path: str, body: bytes = b"") -> bytes:
        status, response = self.request(method, path, body)
        if not 200 <= status < 300:
            raise RuntimeError(
                f"broker {method} {path} returned HTTP {status}: {response!r}"
            )
        return response

    def create(self, command: list[str], cols: int = 80, rows: int = 24) -> str:
        body = json.dumps({"cmd": command, "cols": cols, "rows": rows}).encode()
        response = json.loads(self.ok("POST", "/v1/sessions", body))
        return str(response["id"])

    def kill(self, session_id: str) -> None:
        self.ok("DELETE", f"/v1/sessions/{session_id}")


def cold_ready_metric(
    helper: Path, root: Path, env: dict[str, str], runs: int
) -> dict[str, Any]:
    samples = []
    for index in range(runs):
        broker_root = root / f"cold-{index}"
        broker = Broker(helper, broker_root, env)
        samples.append(broker.ready_ms)
        broker.stop()
    return distribution(samples)


def create_metric(broker: Broker, runs: int) -> dict[str, Any]:
    samples = []
    for _ in range(runs):
        started = time.perf_counter_ns()
        session_id = broker.create(["/bin/sh", "-c", "exec sleep 30"])
        samples.append((time.perf_counter_ns() - started) / 1_000_000)
        broker.kill(session_id)
    return distribution(samples)


def wait_for_text(broker: Broker, session_id: str, text: bytes) -> bytes:
    deadline = time.monotonic() + REQUEST_TIMEOUT
    path = f"/v1/sessions/{session_id}/screen?format=text&scrollback=100"
    while time.monotonic() < deadline:
        response = broker.ok("GET", path)
        if text in response:
            return response
    raise RuntimeError(f"timed out waiting for {text!r} in session {session_id}")


def input_to_screen_metric(
    broker: Broker, session_id: str, runs: int
) -> dict[str, Any]:
    samples = []
    for index in range(runs):
        marker = f"BENCH_{index:04d}".encode()
        body = json.dumps({"text": marker.decode() + "\r"}).encode()
        started = time.perf_counter_ns()
        broker.ok("POST", f"/v1/sessions/{session_id}/input", body)
        wait_for_text(broker, session_id, marker)
        samples.append((time.perf_counter_ns() - started) / 1_000_000)
    return distribution(samples)


def snapshot_poll_metric(broker: Broker, session_id: str, runs: int) -> dict[str, Any]:
    path = f"/v1/sessions/{session_id}/screen?format=wire"
    sizes = []

    def fetch() -> None:
        sizes.append(len(broker.ok("GET", path)))

    metric = measured(runs, fetch)
    metric["response_bytes"] = max(sizes)
    return metric


def pid_from_screen(broker: Broker, session_id: str) -> int:
    response = wait_for_text(broker, session_id, b"PID=")
    marker = response.index(b"PID=") + 4
    digits = []
    while marker < len(response) and 48 <= response[marker] <= 57:
        digits.append(response[marker])
        marker += 1
    if not digits:
        raise RuntimeError("session did not report its PID")
    return int(bytes(digits))


def process_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def detach_and_reattach_metrics(
    broker: Broker, session_id: str, runs: int
) -> dict[str, Any]:
    child_pid = pid_from_screen(broker, session_id)
    detach_started = time.perf_counter_ns()
    broker.ok("POST", f"/v1/sessions/{session_id}/detach")
    detach_ms = (time.perf_counter_ns() - detach_started) / 1_000_000
    time.sleep(0.1)
    survived = process_exists(child_pid)
    same_output = f"PID={child_pid}".encode() in broker.ok(
        "GET", f"/v1/sessions/{session_id}/screen?format=text"
    )

    samples = []
    for _ in range(runs):
        started = time.perf_counter_ns()
        broker.ok("POST", f"/v1/sessions/{session_id}/attach")
        broker.ok("GET", f"/v1/sessions/{session_id}/screen?format=wire")
        samples.append((time.perf_counter_ns() - started) / 1_000_000)
        broker.ok("POST", f"/v1/sessions/{session_id}/detach")
    return {
        "detach_ms": detach_ms,
        "child_pid": child_pid,
        "same_pid_alive": survived,
        "same_pid_visible": same_output,
        "hot_reattach": distribution(samples),
    }


def fairness_metric(broker: Broker, runs: int) -> dict[str, Any]:
    noisy = broker.create(["/usr/bin/yes"])
    try:
        time.sleep(0.05)
        metric = measured(runs, lambda: broker.ok("GET", "/v1/health"))
        metric["all_requests_ok"] = True
        return metric
    finally:
        broker.kill(noisy)


def broker_metrics(
    helper: Path, root: Path, env: dict[str, str], runs: int
) -> dict[str, Any]:
    cold = cold_ready_metric(helper, root, env, runs)
    live_root = root / "live"
    broker = Broker(helper, live_root, env)
    persistent = ""
    try:
        create = create_metric(broker, runs)
        persistent = broker.create(
            ["/bin/sh", "-c", "printf 'PID=%d READY\\n' $$; exec /bin/cat"], rows=80
        )
        snapshot_poll = snapshot_poll_metric(broker, persistent, runs)
        input_screen = input_to_screen_metric(broker, persistent, runs)
        durable = detach_and_reattach_metrics(broker, persistent, runs)
        fairness = fairness_metric(broker, max(runs, 50))
        return {
            "cold_ready": cold,
            "scenario_broker_ready_ms": broker.ready_ms,
            "create": create,
            "snapshot_poll": snapshot_poll,
            "input_to_screen": input_screen,
            "detach_and_reattach": durable,
            "fairness_yes_health": fairness,
        }
    finally:
        if persistent:
            try:
                broker.kill(persistent)
            except (OSError, RuntimeError):
                pass
        broker.stop()


def evaluate(result: dict[str, Any]) -> dict[str, Any]:
    candidate = result["binaries"]["candidate"]
    checks = {
        "help_mean_under_5_ms": candidate["help"]["mean"] < 5.0,
        "version_mean_under_5_ms": candidate["version"]["mean"] < 5.0,
        "pty_first_byte_p95_under_15_ms": candidate["pty_first_byte"]["p95"] < 15.0,
        "stripped_under_500_kib": candidate["stripped_bytes"] < 500 * 1024,
    }
    broker = result.get("broker")
    if broker:
        durable = broker["detach_and_reattach"]
        checks["detach_preserves_same_pid"] = (
            durable["same_pid_alive"] and durable["same_pid_visible"]
        )
        checks["yes_fairness_requests_succeed"] = broker["fairness_yes_health"][
            "all_requests_ok"
        ]
    return {"checks": checks, "passed": all(checks.values())}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--baseline-binary", type=Path)
    parser.add_argument("--broker-helper", type=Path)
    parser.add_argument("--runs", type=int, default=DEFAULT_RUNS)
    parser.add_argument("--broker-runs", type=int, default=DEFAULT_BROKER_RUNS)
    parser.add_argument("--tmp-root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--enforce", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    binary = args.binary.resolve()
    if not binary.is_file() or args.runs < 1 or args.broker_runs < 1:
        raise SystemExit("binary must exist and run counts must be positive")
    parent = args.tmp_root.resolve() if args.tmp_root else None
    with tempfile.TemporaryDirectory(prefix="tnytty-bench-", dir=parent) as temp:
        root = Path(temp)
        root.chmod(0o700)
        env = isolated_env(root)
        result: dict[str, Any] = {
            "schema_version": 1,
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "platform": {
                "system": platform.system(),
                "release": platform.release(),
                "machine": platform.machine(),
                "python": platform.python_version(),
            },
            "configuration": {
                "help_version_runs": HELP_VERSION_RUNS,
                "pty_runs": args.runs,
                "broker_runs": args.broker_runs,
                "isolated": True,
            },
            "binaries": {
                "candidate": binary_metrics(binary, env, root / "candidate", args.runs)
            },
        }
        if args.baseline_binary:
            baseline = args.baseline_binary.resolve()
            if not baseline.is_file():
                raise SystemExit("baseline binary does not exist")
            (root / "baseline").mkdir()
            result["binaries"]["baseline"] = binary_metrics(
                baseline, env, root / "baseline", args.runs
            )
        if args.broker_helper:
            helper = args.broker_helper.resolve()
            if not helper.is_file():
                raise SystemExit("broker helper does not exist")
            result["broker"] = broker_metrics(
                helper, root / "broker-metrics", env, args.broker_runs
            )
        result["verdict"] = evaluate(result)
        rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
        sys.stdout.write(rendered)
        if args.output:
            output = args.output.resolve()
            output.parent.mkdir(parents=True, exist_ok=True)
            temporary = output.with_suffix(output.suffix + ".tmp")
            temporary.write_text(rendered)
            os.replace(temporary, output)
        return 0 if result["verdict"]["passed"] or not args.enforce else 1


if __name__ == "__main__":
    raise SystemExit(main())
