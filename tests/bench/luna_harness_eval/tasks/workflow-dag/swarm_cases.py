#!/usr/bin/env python3
"""Offline task fixtures and independent oracles for the live swarm comparison."""

from __future__ import annotations

import argparse
import concurrent.futures
import importlib.util
import itertools
import json
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable


@dataclass(frozen=True)
class Case:
    name: str
    complexity: str
    specification: str
    files: dict[str, str]


CASES = (
    Case(
        "slug",
        "small: one pure function",
        """Implement normalise.py: slugify(text, max_length=32).
The text must be str (otherwise TypeError). max_length must be an int, excluding
bool, in 1..128 (otherwise ValueError). Apply Unicode NFKD normalisation, discard
non-ASCII codepoints, lowercase, replace each run of characters outside a-z/0-9
with one hyphen, strip leading/trailing hyphens, truncate to max_length and remove
any trailing hyphen. If empty, return 'untitled' truncated to max_length. Use only
the Python standard library; do not change the public test. Include your own tests.
""",
        {
            "normalise.py": "def slugify(text, max_length=32):\n    raise NotImplementedError\n",
            "test_public.py": "from normalise import slugify\nassert slugify('Hello, World!') == 'hello-world'\n",
        },
    ),
    Case(
        "ledger",
        "medium: durable state, idempotency, concurrent clients and CLI",
        """Implement ledger.py and ledger_cli.py using only Python's standard library.
Ledger(path) stores an inventory ledger durably at a file path; SQLite is permitted.
Implement apply(event_id, sku, delta), snapshot(), close(), and context-manager
support. event_id and sku must be nonempty strings of at most 64 characters;
delta must be an int excluding bool. Invalid inputs raise ValueError, with no
state change. Stock starts at zero and must never go negative: reject an invalid
adjustment with ValueError without consuming its event_id. Valid apply returns
the resulting stock for that event's sku. Repeating an event_id with identical sku
and delta returns the ORIGINAL event result, even after later changes; a different
sku or delta for the same ID raises ValueError without mutation. snapshot returns
a dict of all successfully touched SKUs, including zero balances. State persists
across close/reopen. Independent Ledger connections, including simultaneous
threads with one connection per thread, must not lose updates or admit duplicates.
An error must leave the connection usable for a later valid operation.

python ledger_cli.py PATH reads JSON objects, one per stdin line, with exactly
keys event_id, sku, delta. For each valid line print one JSON object {"stock": N}.
For each invalid line (malformed JSON/shape/values or failed adjustment), print
{"error":"ValueError"}, continue subsequent lines, and eventually exit 2 if ANY
line failed, otherwise 0. No extra stdout. Empty input exits 0. Do not change the
public test. Add and run your own tests, including persistence and concurrency.
""",
        {
            "ledger.py": "class Ledger:\n    def __init__(self, path):\n        raise NotImplementedError\n",
            "ledger_cli.py": "raise NotImplementedError\n",
            "test_public.py": "import tempfile\nfrom pathlib import Path\nfrom ledger import Ledger\nwith tempfile.TemporaryDirectory() as d:\n    with Ledger(Path(d) / 'ledger.db') as x:\n        assert x.apply('e1', 'apple', 3) == 3\n        assert x.apply('e1', 'apple', 3) == 3\n        assert x.snapshot() == {'apple': 3}\n",
        },
    ),
    Case(
        "workflow",
        "large: validated DAG, deterministic scheduling, retries and failure propagation",
        """Implement workflow.py and workflow_cli.py using only the Python standard library.
simulate(tasks, workers=2, retries=1) executes a deterministic, discrete-time
DAG simulation. Input tasks is a list of 0..32 dictionaries. Each task has required
'id' (ASCII letter then up to 31 letters/digits/underscore/hyphen), 'duration'
(int excluding bool, 1..1000), optional 'depends_on' (list of distinct task IDs,
default []), and optional 'failures' (int excluding bool, 0..8, default 0). Unknown
keys, duplicate IDs, missing/self/duplicate dependencies, cycles, non-list input,
wrong types or bounds raise ValueError BEFORE simulating. workers must be an int
excluding bool in 1..8; retries an int excluding bool in 0..8.

Tasks start at time zero if dependencies permit. At each scheduling time, first
complete ALL running attempts whose finish time equals now, in lexicographic task
ID order. Attempt n fails iff n <= task.failures. After a failed attempt, retry
if n <= retries, else fail terminally. After processing all completions, mark
pending tasks whose dependency has failed or been skipped as skipped; repeat in
lexicographic waves until no further task is skipped. Then fill vacant worker
slots from ready pending tasks in lexicographic ID order (retries compete normally
with other ready tasks). A task becomes ready only after every dependency succeeds.
Each attempt runs for its task's duration. No same-task overlapping attempts.
Advance to the next completion time; stop when all tasks are terminal.

Return {"makespan": integer, "tasks": {ID: {"state": "succeeded"|"failed"|"skipped",
"attempts": integer, "finish": integer}}, "events": [...]}. finish is time of
terminal success/failure/skip. A skipped task has attempts 0. Each event has exactly
{"time": integer, "task": ID, "attempt": integer, "type": TYPE}; TYPE is start,
succeeded, retry, failed, or skipped. Skipped event attempt is 0. Emit completions
(retry/succeeded/failed), then skip waves, then starts, in the specified order.
Empty input returns makespan 0, tasks {}, events []. Do not mutate the input.

python workflow_cli.py reads ONE JSON object from stdin with required tasks and
optional workers/retries (same defaults); unknown keys or invalid input produce
{"error":"ValueError"} and exit 2. Otherwise output the simulation JSON and exit 0.
No extra stdout. Do not change the public test. Add and run your own tests.
""",
        {
            "workflow.py": "def simulate(tasks, workers=2, retries=1):\n    raise NotImplementedError\n",
            "workflow_cli.py": "raise NotImplementedError\n",
            "test_public.py": "from workflow import simulate\nr = simulate([{'id': 'a', 'duration': 2}, {'id': 'b', 'duration': 1, 'depends_on': ['a']}])\nassert r['makespan'] == 3\nassert r['tasks']['b'] == {'state': 'succeeded', 'attempts': 1, 'finish': 3}\n",
        },
    ),
)

COMMON_PROMPT = """Implement the task described in README.md in this workspace. Read the
specification, modify the implementation files, and run relevant tests. Do not
change test_public.py. Use only the Python standard library and local tools; no
network or package installation is needed. Finish with working files, not just a
plan or code in a message. Collaboration is available when it adds value; reconcile
peer evidence and do not leave collaborators running after your final response.
"""


def prepare(case: Case, workspace: Path) -> None:
    workspace.mkdir(parents=True)
    (workspace / "README.md").write_text(case.specification)
    for name, text in case.files.items():
        (workspace / name).write_text(text)
    subprocess.run(["git", "init", "-q", str(workspace)], check=True)
    subprocess.run(["git", "add", "."], cwd=workspace, check=True)
    subprocess.run(
        [
            "git",
            "-c",
            "user.name=Swarm Benchmark",
            "-c",
            "user.email=benchmark@example.invalid",
            "commit",
            "-qm",
            "fixture",
        ],
        cwd=workspace,
        check=True,
    )


class Checks:
    def __init__(self) -> None:
        self.total = 0
        self.failed: list[str] = []

    def run(self, name: str, operation: Callable[[], Any]) -> None:
        self.total += 1
        try:
            operation()
        except Exception as exc:
            self.failed.append(f"{name}: {type(exc).__name__}: {str(exc)[:250]}")

    def equal(self, name: str, got: Any, expected: Any) -> None:
        def compare() -> None:
            if got != expected:
                raise AssertionError(f"{got!r} != {expected!r}")

        self.run(name, compare)

    def raises(
        self, name: str, operation: Callable[[], Any], kind: type[Exception]
    ) -> None:
        def check() -> None:
            try:
                operation()
            except kind:
                return
            raise AssertionError(f"did not raise {kind.__name__}")

        self.run(name, check)

    def result(self) -> dict[str, Any]:
        return {
            "passed": not self.failed,
            "checks": self.total,
            "failed_checks": len(self.failed),
            "failures": self.failed[:15],
        }


def load(workspace: Path, name: str) -> Any:
    spec = importlib.util.spec_from_file_location(name, workspace / f"{name}.py")
    if spec is None or spec.loader is None:
        raise ValueError("missing implementation")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def grade_slug(workspace: Path, checks: Checks) -> None:
    fn = load(workspace, "normalise").slugify
    for text, length, expected in [
        ("Hello, World!", 32, "hello-world"),
        ("  Café Déjà Vu  ", 32, "cafe-deja-vu"),
        ("foo___bar", 32, "foo-bar"),
        ("A--B", 2, "a"),
        ("", 32, "untitled"),
        ("日本語", 32, "untitled"),
        ("***", 3, "unt"),
        ("①²ﬀ", 32, "12ff"),
        ("A\nB\tC", 32, "a-b-c"),
        ("über-Äpfel", 7, "uber-ap"),
        ("-hello-", 1, "h"),
        ("X" * 200, 128, "x" * 128),
    ]:
        checks.run(
            f"slug {text[:16]!r}",
            lambda t=text, n=length, e=expected: checks.equal("value", fn(t, n), e),
        )
    for value in (None, 42, b"bytes", [], {}):
        checks.raises("text type", lambda v=value: fn(v), TypeError)
    for value in (0, -1, 129, True, 1.5, "12", None):
        checks.raises("length type/bound", lambda v=value: fn("valid", v), ValueError)


def grade_ledger(workspace: Path, checks: Checks) -> None:
    cls = load(workspace, "ledger").Ledger
    with tempfile.TemporaryDirectory(prefix="tny-ledger-oracle-") as tmp:
        path = Path(tmp) / "state.db"
        with cls(path) as ledger:
            checks.equal("initial", ledger.snapshot(), {})
            checks.equal("apply", ledger.apply("e1", "apple", 5), 5)
            checks.equal("decrement", ledger.apply("e2", "apple", -2), 3)
            checks.equal("original duplicate result", ledger.apply("e1", "apple", 5), 5)
            checks.raises(
                "conflicting delta", lambda: ledger.apply("e1", "apple", 6), ValueError
            )
            checks.raises(
                "conflicting sku", lambda: ledger.apply("e1", "pear", 5), ValueError
            )
            checks.raises(
                "negative stock", lambda: ledger.apply("retry", "pear", -1), ValueError
            )
            checks.equal("rejected id reusable", ledger.apply("retry", "pear", 2), 2)
            for index, args in enumerate(
                [
                    ("", "x", 1),
                    ("x" * 65, "x", 1),
                    ("bad", "", 1),
                    ("bad", "x", True),
                    ("bad", "x", 1.0),
                    (None, "x", 1),
                ]
            ):
                checks.raises(
                    f"invalid {index}", lambda a=args: ledger.apply(*a), ValueError
                )
            checks.equal("after rejections", ledger.snapshot(), {"apple": 3, "pear": 2})
            ledger.apply("zero", "empty", 0)
        with cls(path) as ledger:
            checks.equal(
                "reopen", ledger.snapshot(), {"apple": 3, "pear": 2, "empty": 0}
            )
            checks.equal("persist duplicate", ledger.apply("e1", "apple", 5), 5)

        def writer(worker: int) -> None:
            with cls(path) as item:
                for index in range(12):
                    item.apply(f"w{worker}-{index}", "counter", 1)
                    item.apply(f"w{worker}-{index}", "counter", 1)

        def concurrent_writers() -> None:
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(writer, range(4)))
            with cls(path) as ledger:
                if ledger.snapshot().get("counter") != 48:
                    raise AssertionError("lost update or duplicate application")

        checks.run("independent concurrent connections", concurrent_writers)
        lines = [
            json.dumps({"event_id": "cli1", "sku": "cli", "delta": 2}),
            "not json",
            json.dumps({"event_id": "cli2", "sku": "cli", "delta": -9}),
            json.dumps({"event_id": "cli2", "sku": "cli", "delta": 3}),
            json.dumps({"event_id": "cli1", "sku": "cli", "delta": 2, "extra": 1}),
        ]
        p = subprocess.run(
            [sys.executable, str(workspace / "ledger_cli.py"), str(path)],
            input="\n".join(lines) + "\n",
            text=True,
            capture_output=True,
            timeout=15,
        )
        checks.equal("CLI mixed exit", p.returncode, 2)
        checks.equal(
            "CLI mixed output",
            [json.loads(x) for x in p.stdout.splitlines()],
            [
                {"stock": 2},
                {"error": "ValueError"},
                {"error": "ValueError"},
                {"stock": 5},
                {"error": "ValueError"},
            ],
        )


def reference_schedule(
    tasks: list[dict[str, Any]], workers: int, retries: int
) -> dict[str, Any]:
    """Small independent tick-based oracle; production solutions need not use ticks."""
    specs = {item["id"]: item for item in tasks}
    states = {name: "pending" for name in specs}
    attempts = dict.fromkeys(specs, 0)
    finish: dict[str, int] = {}
    active: dict[str, int] = {}
    events: list[dict[str, Any]] = []
    now = 0

    def event(name: str, kind: str) -> None:
        events.append(
            {"time": now, "task": name, "attempt": attempts[name], "type": kind}
        )

    while True:
        for name in sorted(key for key, end in active.items() if end == now):
            del active[name]
            if attempts[name] <= specs[name].get("failures", 0):
                states[name] = "pending" if attempts[name] <= retries else "failed"
                event(name, "retry" if states[name] == "pending" else "failed")
            else:
                states[name] = "succeeded"
                event(name, "succeeded")
            if states[name] != "pending":
                finish[name] = now
        while True:
            skip = sorted(
                name
                for name in specs
                if states[name] == "pending"
                and any(
                    states[dep] in ("failed", "skipped")
                    for dep in specs[name].get("depends_on", [])
                )
            )
            if not skip:
                break
            for name in skip:
                states[name] = "skipped"
                finish[name] = now
                event(name, "skipped")
        for name in sorted(specs):
            if len(active) >= workers:
                break
            if states[name] == "pending" and all(
                states[d] == "succeeded" for d in specs[name].get("depends_on", [])
            ):
                attempts[name] += 1
                states[name] = "running"
                active[name] = now + specs[name]["duration"]
                event(name, "start")
        if all(
            state in ("failed", "skipped", "succeeded") for state in states.values()
        ):
            return {
                "makespan": now,
                "tasks": {
                    name: {
                        "state": states[name],
                        "attempts": attempts[name],
                        "finish": finish[name],
                    }
                    for name in specs
                },
                "events": events,
            }
        now += 1
        if now > 2000:
            raise AssertionError("oracle workload bound exceeded")


def grade_workflow(workspace: Path, checks: Checks) -> None:
    simulate = load(workspace, "workflow").simulate
    checks.equal("empty", simulate([]), {"makespan": 0, "tasks": {}, "events": []})
    # Enumerate dependency/failure/worker variants rather than testing a single happy path.
    edges = [("a", "b"), ("a", "c"), ("b", "c")]
    for mask, failures, workers, retries in itertools.product(
        range(8), (0, 1, 2), (1, 2), (0, 1)
    ):
        tasks = [
            {
                "id": name,
                "duration": index + 1,
                "failures": failures if name == "a" else 0,
                "depends_on": [
                    left
                    for bit, (left, right) in enumerate(edges)
                    if right == name and mask & (1 << bit)
                ],
            }
            for index, name in enumerate(("a", "b", "c"))
        ]
        original = json.dumps(tasks, sort_keys=True)

        def check(ts=tasks, ws=workers, rs=retries, before=original) -> None:
            actual = simulate(ts, workers=ws, retries=rs)
            expected = reference_schedule(ts, ws, rs)
            if actual != expected:
                raise AssertionError(f"different schedule: {actual!r}")
            if json.dumps(ts, sort_keys=True) != before:
                raise AssertionError("input mutated")

        checks.run(
            f"DAG mask={mask},failures={failures},workers={workers},retries={retries}",
            check,
        )
    invalid = [
        None,
        {},
        [{"id": "a", "duration": True}],
        [{"id": "a", "duration": 0}],
        [{"id": "a", "duration": 1, "extra": 0}],
        [{"id": "a", "duration": 1, "depends_on": ["missing"]}],
        [{"id": "a", "duration": 1}, {"id": "a", "duration": 1}],
        [
            {"id": "a", "duration": 1, "depends_on": ["b"]},
            {"id": "b", "duration": 1, "depends_on": ["a"]},
        ],
        [{"id": "a", "duration": 1, "depends_on": ["a"]}],
        [{"id": "9bad", "duration": 1}],
        [{"id": "a", "duration": 1, "failures": -1}],
    ]
    for index, value in enumerate(invalid):
        checks.raises(f"invalid task {index}", lambda v=value: simulate(v), ValueError)
    for field, value in [
        ("workers", True),
        ("workers", 0),
        ("workers", 9),
        ("retries", True),
        ("retries", -1),
        ("retries", 9),
    ]:
        checks.raises(
            f"invalid {field}",
            lambda f=field, v=value: simulate([], **{f: v}),
            ValueError,
        )
    for request, code in [
        ({"tasks": []}, 0),
        ({"tasks": [], "unknown": 1}, 2),
        ({"tasks": [], "workers": 0}, 2),
    ]:
        p = subprocess.run(
            [sys.executable, str(workspace / "workflow_cli.py")],
            input=json.dumps(request),
            text=True,
            capture_output=True,
            timeout=10,
        )
        checks.equal("CLI exit", p.returncode, code)
        checks.equal(
            "CLI value",
            json.loads(p.stdout),
            {"error": "ValueError"}
            if code
            else {"makespan": 0, "tasks": {}, "events": []},
        )


def grade(name: str, workspace: Path) -> dict[str, Any]:
    checks = Checks()
    checks.run(
        "load and execute oracle", lambda: globals()[f"grade_{name}"](workspace, checks)
    )
    return checks.result()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--grade", choices=[case.name for case in CASES], required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(grade(args.grade, args.workspace.resolve()), sort_keys=True))


if __name__ == "__main__":
    main()
