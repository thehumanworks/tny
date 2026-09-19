#!/usr/bin/env python3
"""Bounded task-body improvement, an optional Python >=3.9 workflow.

Commands and the run store MUST be trusted. The evaluator is fixed user code,
not the proposer's self-report. Hashes detect corruption, not a forged archive.
Commands run without a shell, in the spec directory, with the inherited
environment; this is NOT a sandbox. Only training feedback is sent to the
proposer. Cost is the evaluator's declared metric, not inferred API spend.

Public API: run(spec_path, out_directory) -> report; promote(run_directory,
target, expected_sha256) -> report. Both raise ImprovementError on failure.
Run failures retain bounded raw evidence and a failed report. No run activates
instructions. Promotion replays archived evidence without executing commands.
The baseline.md archive is the rollback copy. Serialize promotion with other
target writers; portable atomic replace is not a filesystem compare-and-swap.
Native Python only; this optional workflow is not part of the wasm/C runtime.
"""

from __future__ import annotations

import argparse
import errno
import hashlib
import json
import math
import os
import selectors
import signal
import stat
import subprocess
import sys
import tempfile
import time
from fractions import Fraction
from pathlib import Path
from typing import Any

MAX_JSON = 1024 * 1024
MAX_BODY = 64 * 1024


class ImprovementError(ValueError):
    """Invalid configuration, failed evidence, or unsafe promotion."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ImprovementError(message)


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _dump(value: Any) -> bytes:
    return (
        json.dumps(value, ensure_ascii=False, allow_nan=False, sort_keys=True) + "\n"
    ).encode("utf-8")


def _pairs(items: list) -> dict:
    result = {}
    for key, value in items:
        _require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def _constant(value: str) -> None:
    raise ImprovementError("nonfinite JSON number: " + value)


def _float(value: str) -> float:
    number = float(value)
    _require(math.isfinite(number), "nonfinite JSON number")
    return number


def _load(data: bytes) -> Any:
    _require(len(data) <= MAX_JSON, "JSON exceeds 1 MiB")
    try:
        return json.loads(
            data.decode("utf-8"),
            object_pairs_hook=_pairs,
            parse_constant=_constant,
            parse_float=_float,
        )
    except (UnicodeError, ValueError, RecursionError) as exc:
        raise ImprovementError("invalid JSON: " + str(exc)) from exc


def _read(path: Path, limit: int = MAX_JSON) -> bytes:
    _require(
        not path.is_symlink() and path.is_file(), "not a regular file: " + str(path)
    )
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    _require(len(data) <= limit, "file exceeds size bound: " + str(path))
    return data


def _body(value: Any) -> str:
    _require(
        isinstance(value, str) and bool(value.strip()),
        "instructions must be nonempty text",
    )
    try:
        _require(len(value.encode("utf-8")) <= MAX_BODY, "instructions exceed 64 KiB")
    except UnicodeError as exc:
        raise ImprovementError("instructions must be UTF-8") from exc
    _require("\0" not in value, "instructions contain NUL")
    lines = value.lstrip("\ufeff \t\r\n").splitlines()
    _require(bool(lines), "instructions must contain a task body")
    first = lines[0].strip()
    _require(
        first not in ("---", "+++"),
        "instructions must be task body, without frontmatter",
    )
    return value


def _number(value: Any) -> bool:
    try:
        return type(value) in (int, float) and math.isfinite(value) and value >= 0
    except OverflowError:
        return False


def _spec(value: Any) -> dict:
    _require(isinstance(value, dict), "spec must be an object")
    _require(
        type(value.get("version")) is int and value["version"] == 1,
        "spec version must be 1",
    )
    _require(
        isinstance(value.get("baseline"), str) and bool(value["baseline"]),
        "baseline path required",
    )
    for name in ("proposer", "evaluator"):
        argv = value.get(name)
        _require(
            isinstance(argv, list)
            and bool(argv)
            and all(
                isinstance(arg, str) and bool(arg) and "\0" not in arg for arg in argv
            ),
            name + " must be a nonempty argv list",
        )
    _require(
        type(value.get("rounds")) is int and 1 <= value["rounds"] <= 20,
        "rounds must be 1..20",
    )
    timeout = value.get("timeout_s")
    _require(
        _number(timeout) and 0 < timeout <= 3600, "timeout_s must be >0 and <=3600"
    )
    _require(
        isinstance(value.get("cost_unit"), str) and bool(value["cost_unit"].strip()),
        "cost_unit required",
    )
    cases = value.get("cases")
    _require(
        isinstance(cases, dict) and set(cases) == {"train", "validation", "test"},
        "three case splits required",
    )
    seen = set()
    input_splits = {}
    for split, entries in cases.items():
        _require(
            isinstance(entries, list) and bool(entries),
            split + " cases must be nonempty",
        )
        for case in entries:
            _require(
                isinstance(case, dict)
                and isinstance(case.get("id"), str)
                and bool(case["id"].strip())
                and "input" in case,
                "case requires id and input",
            )
            _require(
                case["id"] not in seen, "duplicate case id across splits: " + case["id"]
            )
            seen.add(case["id"])
            digest = _sha(_dump(case["input"]))
            _require(
                input_splits.get(digest, split) == split,
                "duplicate input across splits",
            )
            input_splits[digest] = split
    return value


def _evaluation(value: Any) -> dict:
    _require(
        isinstance(value, dict) and set(value) == {"passed", "cost", "feedback"},
        "invalid evaluator fields",
    )
    _require(type(value["passed"]) is bool, "passed must be boolean")
    _require(
        _number(value["cost"]), "cost must be nonnegative finite number (not bool)"
    )
    _require(isinstance(value["feedback"], str), "feedback must be text")
    return value


def _proposal(value: Any) -> dict:
    _require(
        isinstance(value, dict) and set(value) == {"instructions", "rationale"},
        "invalid proposer fields",
    )
    _body(value["instructions"])
    _require(isinstance(value["rationale"], str), "rationale must be text")
    return value


def _total(results: list) -> Fraction:
    # Exact sums of the declared JSON values: float rounding must not hide a
    # small cost increase next to a large value, or invent a strict improvement.
    return sum((Fraction(item["cost"]) for item in results), Fraction())


def gate(parent: dict, candidate: dict) -> tuple:
    """Pareto selection on train/validation only; return (accepted, reason)."""
    for split in ("train", "validation"):
        before, after = parent[split], candidate[split]
        _require(len(before) == len(after), "case count changed")
        if any(old["passed"] and not new["passed"] for old, new in zip(before, after)):
            return False, split + " pass regression"
        if _total(after) > _total(before):
            return False, split + " cost increase"
    old, new = parent["train"], candidate["train"]
    improved = sum(item["passed"] for item in new) > sum(item["passed"] for item in old)
    if improved or _total(new) < _total(old):
        return True, "train improvement; both splits Pareto-safe"
    return False, "no strict train improvement"


def _drain(
    process: subprocess.Popen, output: bytearray, errors: bytearray, seconds: float
) -> None:
    """Observe pipe EOF, not just leader exit, while descendants clean up."""
    deadline = time.monotonic() + seconds
    with selectors.DefaultSelector() as selector:
        for stream, buffer in ((process.stdout, output), (process.stderr, errors)):
            if not stream.closed:
                selector.register(stream, selectors.EVENT_READ, buffer)
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            for key, _ in selector.select(remaining):
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    selector.unregister(key.fileobj)
                else:
                    key.data.extend(chunk[: MAX_JSON - len(key.data)])


def _stop(process: subprocess.Popen, output: bytearray, errors: bytearray) -> str:
    # Descendants of a wrapper retain these pipes to publish cancellation
    # receipts. A reaped leader alone does not end their TERM grace period.
    warnings = []
    for sig, grace in ((signal.SIGTERM, 20), (signal.SIGKILL, 5)):
        started = time.monotonic()
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            pass
        except OSError as exc:
            warnings.append(str(exc))
        try:
            _drain(process, output, errors, grace)
            process.wait(timeout=max(0, grace - (time.monotonic() - started)))
        except subprocess.TimeoutExpired:
            if sig == signal.SIGKILL:
                warnings.append("child exit unverified after cancellation")
        except OSError as exc:
            warnings.append(str(exc))
    return "; ".join(warnings)


class _Evidence:
    def __init__(self, root: Path, cwd: str, replay: bool = False):
        self.root, self.cwd, self.replay = root, cwd, replay
        self.used = set()

    def put(self, name: str, data: bytes) -> None:
        _require(len(data) <= MAX_JSON, "archive record exceeds 1 MiB: " + name)
        self.used.add(name)
        path = self.root / name
        if self.replay:
            _require(_read(path) == data, "archive evidence mismatch: " + name)
        else:
            with path.open("xb") as stream:
                stream.write(data)

    def call(self, name: str, argv: list, request: dict, timeout: float) -> Any:
        raw = _dump(request)
        _require(len(raw) <= MAX_JSON, "command request exceeds 1 MiB")
        self.put(name + ".input.json", raw)
        names = [name + suffix for suffix in (".stdout", ".stderr", ".command.json")]
        self.used.update(names)
        command = {"argv": argv, "cwd": self.cwd, "timeout_s": timeout}
        if self.replay:
            meta = _load(_read(self.root / names[2]))
            _require(
                meta == dict(command, returncode=0, error=None),
                "invalid command evidence: " + name,
            )
            return _load(_read(self.root / names[0]))
        output, errors = bytearray(), bytearray()
        process = None
        error = None
        code = None
        deadline = time.monotonic() + timeout
        try:
            with (self.root / (name + ".input.json")).open("rb") as source:
                process = subprocess.Popen(
                    argv,
                    stdin=source,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    cwd=self.cwd,
                    env=dict(os.environ, TNY_IMPROVE_TIMEOUT_S=str(timeout)),
                    shell=False,
                    start_new_session=os.name == "posix",
                )
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ, output)
                selector.register(process.stderr, selectors.EVENT_READ, errors)
                while selector.get_map():
                    remaining = deadline - time.monotonic()
                    _require(remaining > 0, "command timeout: " + name)
                    for key, _ in selector.select(remaining):
                        chunk = os.read(key.fileobj.fileno(), 65536)
                        if not chunk:
                            selector.unregister(key.fileobj)
                            continue
                        buffer = key.data
                        room = MAX_JSON - len(buffer)
                        buffer.extend(chunk[:room])
                        _require(
                            len(chunk) <= room, "command output exceeds 1 MiB: " + name
                        )
                remaining = deadline - time.monotonic()
                _require(remaining > 0, "command timeout: " + name)
                code = process.wait(timeout=remaining)
                _require(code == 0, "command exited nonzero: " + name)
        except BaseException as exc:
            error = str(exc) or type(exc).__name__
            raise
        finally:
            # A FIRST interrupt can arrive after a timeout already entered
            # cleanup. Block it until child cleanup AND evidence writes finish.
            previous_mask = signal.pthread_sigmask(
                signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM}
            )
            try:
                if process is not None:
                    try:
                        if error is not None:
                            cleanup_error = _stop(process, output, errors)
                            if cleanup_error:
                                error += "; cleanup: " + cleanup_error
                        code = process.returncode
                    finally:
                        process.stdout.close()
                        process.stderr.close()
                self.put(names[0], bytes(output))
                self.put(names[1], bytes(errors))
                self.put(names[2], _dump(dict(command, returncode=code, error=error)))
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
        return _load(bytes(output))


def _search(spec: dict, baseline: str, evidence: _Evidence) -> dict:
    def evaluate(body: str, label: str, splits: tuple) -> dict:
        result = {}
        for split in splits:
            result[split] = [
                _evaluation(
                    evidence.call(
                        f"{label}.{split}.{index:04d}",
                        spec["evaluator"],
                        {"instructions": body, "case": case},
                        spec["timeout_s"],
                    )
                )
                for index, case in enumerate(spec["cases"][split])
            ]
            _total(result[split])
        return result

    parent = baseline
    scores = evaluate(parent, "baseline", ("train", "validation"))
    initial = scores
    rounds = []
    accepted_count = 0
    for index in range(1, spec["rounds"] + 1):
        label = f"round-{index:02d}"
        feedback = [
            dict(result, id=case["id"])
            for case, result in zip(spec["cases"]["train"], scores["train"])
        ]
        proposal = _proposal(
            evidence.call(
                label + ".proposal",
                spec["proposer"],
                {
                    "parent_instructions": parent,
                    "round": index,
                    "training_feedback": feedback,
                },
                spec["timeout_s"],
            )
        )
        body = proposal["instructions"]
        evidence.put(label + ".md", body.encode("utf-8"))
        candidate = evaluate(body, label, ("train", "validation"))
        accepted, reason = gate(scores, candidate)
        if body == parent:
            accepted, reason = False, "identical instructions"
        decision = {
            "round": index,
            "parent_sha256": _sha(parent.encode("utf-8")),
            "candidate_sha256": _sha(body.encode("utf-8")),
            "accepted": accepted,
            "reason": reason,
            "rationale": proposal["rationale"],
            "results": candidate,
        }
        evidence.put(label + ".decision.json", _dump(decision))
        rounds.append(decision)
        if accepted:
            parent, scores = body, candidate
            accepted_count += 1
    # The holdout is observed only after all decisions. It never changes eligibility.
    holdout = {
        "baseline": evaluate(baseline, "holdout-baseline", ("test",))["test"],
        "final": evaluate(parent, "holdout-final", ("test",))["test"],
    }
    evidence.put("final.md", parent.encode("utf-8"))
    return {
        "version": 1,
        "status": "complete",
        "eligible": accepted_count > 0,
        "accepted_count": accepted_count,
        "baseline_sha256": _sha(baseline.encode("utf-8")),
        "final_sha256": _sha(parent.encode("utf-8")),
        "cost_unit": spec["cost_unit"],
        "baseline_results": initial,
        "rounds": rounds,
        "holdout": holdout,
        "holdout_selection_evidence": False,
    }


def _seal(root: Path) -> None:
    hashes = {
        path.name: _sha(_read(path))
        for path in sorted(root.iterdir())
        if path.name != "manifest.json"
    }
    with (root / "manifest.json").open("xb") as stream:
        stream.write(_dump({"version": 1, "sha256": hashes}))


def run(spec_path: Any, out_directory: Any) -> dict:
    """Create an exclusive 0700 archive and run bounded search, never activation."""
    _require(os.name == "posix", "instruction evolution requires native POSIX Python")
    root = Path(out_directory).absolute()
    try:
        root.mkdir(mode=0o700)
    except OSError as exc:
        raise ImprovementError("output must be a NEW directory: " + str(exc)) from exc
    try:
        spec_path = Path(spec_path).absolute()
        raw_spec = _read(spec_path)
        evidence = _Evidence(root, str(spec_path.parent.resolve()))
        evidence.put("spec.json", raw_spec)
        spec = _spec(_load(raw_spec))
        baseline_raw = _read(spec_path.parent / spec["baseline"], MAX_BODY)
        evidence.put("baseline.md", baseline_raw)
        baseline = _body(baseline_raw.decode("utf-8"))
        evidence.put("inputs.json", _dump(spec["cases"]))
        evidence.put("context.json", _dump({"cwd": evidence.cwd}))
        report = _search(spec, baseline, evidence)
        evidence.put("report.json", _dump(report))
        _seal(root)
        return report
    except (Exception, KeyboardInterrupt) as exc:
        report = {
            "version": 1,
            "status": "failed",
            "eligible": False,
            "error": (str(exc) or type(exc).__name__)[:4096],
        }
        (root / "report.json").write_bytes(_dump(report))
        if not (root / "manifest.json").exists():
            _seal(root)
        raise ImprovementError(report["error"]) from exc


def _no_symlinks(path: Path) -> None:
    for component in (path, *path.parents):
        _require(not component.is_symlink(), "symlinks are refused: " + str(component))


def _verify(root: Path) -> tuple:
    _no_symlinks(root)
    manifest = _load(_read(root / "manifest.json"))
    _require(
        isinstance(manifest, dict)
        and manifest.get("version") == 1
        and isinstance(manifest.get("sha256"), dict),
        "invalid manifest",
    )
    hashes = manifest["sha256"]
    _require(
        set(hashes) == {path.name for path in root.iterdir()} - {"manifest.json"},
        "archive inventory mismatch",
    )
    for name, digest in hashes.items():
        _require(
            Path(name).name == name and name not in (".", ".."), "invalid archive path"
        )
        _require(_sha(_read(root / name)) == digest, "archive hash mismatch: " + name)
    report = _load(_read(root / "report.json"))
    _require(
        isinstance(report, dict)
        and report.get("status") == "complete"
        and report.get("eligible") is True,
        "run is incomplete or has no accepted candidate",
    )
    spec_raw = _read(root / "spec.json")
    spec = _spec(_load(spec_raw))
    context = _load(_read(root / "context.json"))
    _require(
        isinstance(context, dict) and isinstance(context.get("cwd"), str),
        "invalid archive context",
    )
    evidence = _Evidence(root, context["cwd"], replay=True)
    baseline_raw = _read(root / "baseline.md", MAX_BODY)
    evidence.put("spec.json", spec_raw)
    evidence.put("baseline.md", baseline_raw)
    evidence.put("inputs.json", _dump(spec["cases"]))
    evidence.put("context.json", _dump(context))
    rebuilt = _search(spec, _body(baseline_raw.decode("utf-8")), evidence)
    evidence.put("report.json", _dump(rebuilt))
    _require(evidence.used == set(hashes), "unexpected archive evidence")
    return rebuilt, _read(root / "final.md", MAX_BODY)


def _fsync_directory(fd: int) -> None:
    try:
        os.fsync(fd)
    except OSError as exc:
        if exc.errno not in (errno.EINVAL, errno.ENOTSUP):
            raise


def promote(run_directory: Any, target: Any, expected_sha256: str) -> dict:
    """Verify all archived evidence, then explicitly replace an unchanged baseline.

    Requires a trusted archive and serialized target writers. Refuses symlinks
    (including parents). Keeps target mode and fsyncs file and parent directory.
    """
    temporary = None
    try:
        root, target = Path(run_directory).absolute(), Path(target).absolute()
        _no_symlinks(root)
        _no_symlinks(target)
        # Preserve symlink refusal on the supplied paths, then normalize '..'
        # before checking containment. Never overwrite the rollback archive.
        root, target = root.resolve(strict=True), target.resolve(strict=True)
        report, body = _verify(root)
        _require(
            expected_sha256 == report["baseline_sha256"],
            "expected digest is not the run baseline",
        )
        _no_symlinks(target)
        _require(root not in target.parents, "target must be outside the run archive")
        before = target.stat()
        _require(
            stat.S_ISREG(before.st_mode), "target must be an existing regular file"
        )
        _require(
            _sha(_read(target, MAX_BODY)) == expected_sha256,
            "stale target: baseline digest changed",
        )
        parent_fd = os.open(
            target.parent,
            os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0),
        )
        try:
            fd, temporary = tempfile.mkstemp(prefix=".tny-improve-", dir=target.parent)
            with os.fdopen(fd, "wb") as stream:
                stream.write(body)
                stream.flush()
                os.fchmod(stream.fileno(), stat.S_IMODE(before.st_mode))
                os.fsync(stream.fileno())
            _no_symlinks(target)
            after = target.stat()
            _require(
                (
                    before.st_dev,
                    before.st_ino,
                    before.st_mode,
                    before.st_mtime_ns,
                    before.st_ctime_ns,
                )
                == (
                    after.st_dev,
                    after.st_ino,
                    after.st_mode,
                    after.st_mtime_ns,
                    after.st_ctime_ns,
                )
                and _sha(_read(target, MAX_BODY)) == expected_sha256,
                "target changed during promotion",
            )
            os.replace(temporary, target)
            temporary = None
            _fsync_directory(parent_fd)
        finally:
            os.close(parent_fd)
        return {
            "status": "promoted",
            "target": str(target),
            "sha256": report["final_sha256"],
            "rollback": str(root / "baseline.md"),
        }
    except (OSError, ValueError, KeyError, TypeError, UnicodeError) as exc:
        raise ImprovementError(str(exc)) from exc
    finally:
        if temporary is not None:
            os.unlink(temporary)


def _interrupt(_signum: int, _frame: Any) -> None:
    # A repeated interrupt must not cut short the bounded child cleanup.
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    raise KeyboardInterrupt("instruction experiment interrupted")


def main(argv: Any = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    commands = parser.add_subparsers(dest="command", required=True)
    start = commands.add_parser(
        "run", help="search offline via trusted configured commands; no activation"
    )
    start.add_argument("--spec", required=True, type=Path)
    start.add_argument("--out", required=True, type=Path)
    activate = commands.add_parser(
        "promote", help="verify trusted archive and replace an unchanged baseline"
    )
    activate.add_argument("--run", required=True, type=Path)
    activate.add_argument("--target", required=True, type=Path)
    activate.add_argument("--expected-sha256", required=True)
    args = parser.parse_args(argv)
    previous = (
        {sig: signal.signal(sig, _interrupt) for sig in (signal.SIGINT, signal.SIGTERM)}
        if os.name == "posix"
        else {}
    )
    try:
        result = (
            run(args.spec, args.out)
            if args.command == "run"
            else promote(args.run, args.target, args.expected_sha256)
        )
        print(_dump(result).decode("utf-8"), end="")
        return 0
    except (ImprovementError, KeyboardInterrupt) as exc:
        print(_dump({"status": "failed", "error": str(exc)}).decode("utf-8"), end="")
        return 1
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


if __name__ == "__main__":
    sys.exit(main())
