#!/usr/bin/env python3
"""Code generation: decompose, architect, generate, verify, learn.

    decompose ──► architecture ──► gen-<unit> ... (a DAG built from the
        ▲                           │   decomposition; units run in parallel
        │                           ▼   and receive their upstream reports)
        │                         review
        │                           │
        │          verify command ◄─┴─► fix ──► verify ... (bounded)
        └── lessons from earlier runs ◄── retro

The graph is not known until the decomposer replies, so the script builds the
`Workflow` from that reply. Verification is a command *you* pass with
`--verify`; it runs on the host, outside any agent, and its exit status is the
only thing that counts as passing. Generators and the fixer may write and run
commands inside `--workspace`; every other role is denied.

    OPENAI_API_KEY=... python3 codegen.py "A CLI that prints a greeting" \\
        --workspace /tmp/greeter --verify "python3 main.py Ada | grep -q 'Hello, Ada!'"
"""

from __future__ import annotations

import argparse
import asyncio
import json
import re
import subprocess
from pathlib import Path, PurePosixPath
from typing import Any

import tny
from _common import (
    Ledger,
    Lessons,
    Roles,
    add_runtime_arguments,
    ask,
    ask_json,
    log,
    log_event,
    permission_policy,
    require,
    run,
    string_list,
    text_of,
    validate_lessons,
)

_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("spec", help="the specification, or @FILE to read it from")
    parser.add_argument(
        "--verify", help="shell command that must exit 0, run in --workspace"
    )
    parser.add_argument(
        "--verify-timeout", type=int, default=600, help="seconds (default: 600)"
    )
    parser.add_argument(
        "--max-repairs", type=int, default=2, help="fix attempts (default: 2)"
    )
    parser.add_argument(
        "--max-units", type=int, default=6, help="work units (default: 6)"
    )
    add_runtime_arguments(parser)
    parser.set_defaults(workspace="codegen-out")
    return parser.parse_args()


def validate_units(limit: int) -> Any:
    def validate(value: Any) -> list[dict[str, Any]]:
        require(isinstance(value, dict), "reply must be a JSON object")
        raw = value.get("units")
        require(isinstance(raw, list) and bool(raw), "units must be a non-empty list")
        require(len(raw) <= limit, f"at most {limit} units are allowed")
        units: list[dict[str, Any]] = []
        owners: dict[str, str] = {}
        for item in raw:
            require(isinstance(item, dict), "each unit must be an object")
            identifier, goal = item.get("id"), item.get("goal")
            require(
                isinstance(identifier, str) and bool(_ID.match(identifier)),
                "each id must be a short slug of letters, digits, '-' or '_'",
            )
            require(isinstance(goal, str) and bool(goal.strip()), "goal must be text")
            require(all(identifier != u["id"] for u in units), "ids must be unique")
            files = string_list(item.get("files"), f"{identifier}.files", limit=64)
            require(bool(files), f"unit {identifier} must own at least one file")
            for name in files:
                path = PurePosixPath(name)
                require(
                    not path.is_absolute() and ".." not in path.parts,
                    f"{name} must be a relative path inside the workspace",
                )
                # Parallel generators share one workspace, so file ownership
                # has to be disjoint; the decomposer repairs an overlap.
                require(
                    name not in owners,
                    f"{name} is owned by both {owners.get(name)} and {identifier}",
                )
                owners[name] = identifier
            acceptance = item.get("acceptance")
            units.append(
                {
                    "id": identifier,
                    "goal": goal.strip(),
                    "files": files,
                    "depends_on": string_list(
                        item.get("depends_on", []), f"{identifier}.depends_on", limit=16
                    ),
                    "acceptance": acceptance.strip()
                    if isinstance(acceptance, str)
                    else "",
                }
            )
        known = {unit["id"] for unit in units}
        for unit in units:
            for dependency in unit["depends_on"]:
                require(
                    dependency in known and dependency != unit["id"],
                    f"{unit['id']} depends on unknown unit {dependency}",
                )
        # Report a cycle to the decomposer rather than letting the workflow
        # reject the graph after the model's turn is over.
        done: set[str] = set()
        while len(done) < len(units):
            ready = [
                u["id"]
                for u in units
                if u["id"] not in done and set(u["depends_on"]) <= done
            ]
            require(bool(ready), "unit dependencies contain a cycle")
            done.update(ready)
        return units

    return validate


def build_workflow(
    roles: Roles, spec: str, units: list[dict[str, Any]], advice: str, jobs: int
) -> tny.Workflow:
    workflow = tny.Workflow(
        roles.config("codegen-generator", mode="auto"),
        max_concurrency=jobs,
        on_event=log_event,
        # `auto` already allows edits inside the workspace; this additionally
        # lets generators run their checks. Architect and reviewer override
        # the runtime below and stay read-only.
        on_permission=permission_policy(lambda task: task.startswith("gen-")),
    )
    workflow.task(
        "architecture",
        f"Specification:\n{spec}\n\nWork units:\n{json.dumps(units, indent=2)}{advice}",
        runtime_config=roles.config("codegen-architect"),
    )
    for unit in units:
        acceptance = f"\nAcceptance: {unit['acceptance']}" if unit["acceptance"] else ""
        workflow.task(
            f"gen-{unit['id']}",
            f"Specification:\n{spec}\n\nUnit `{unit['id']}`: {unit['goal']}\n"
            f"Files you own: {', '.join(unit['files'])}{acceptance}",
            depends_on=[
                "architecture",
                *(f"gen-{dependency}" for dependency in unit["depends_on"]),
            ],
        )
    workflow.task(
        "review",
        "Review the generated code in this workspace against the specification "
        "and the architecture contract. The generator reports below are claims "
        f"to check, not facts.\n\nSpecification:\n{spec}",
        depends_on=["architecture", *(f"gen-{unit['id']}" for unit in units)],
        runtime_config=roles.config("review"),  # built-in preset
    )
    return workflow


def run_verify(command: str, workspace: Path, timeout: int) -> tuple[bool, str]:
    try:
        done = subprocess.run(
            command,
            shell=True,
            cwd=workspace,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return False, f"timed out after {timeout}s"
    output = (done.stdout + done.stderr).strip()
    return done.returncode == 0, f"exit {done.returncode}\n{output[-4000:]}"


async def main() -> int:
    args = parse_arguments()
    spec = (
        Path(args.spec[1:]).read_text(encoding="utf-8")
        if args.spec.startswith("@")
        else args.spec
    )
    workspace = Path(args.workspace).resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    roles, ledger = Roles(args), Ledger()
    lessons = Lessons.for_workflow(args, "codegen")
    recalled = lessons.load()
    log(
        f"workspace {workspace}; recalled {len(recalled)} lesson(s) from {lessons.path}"
    )

    units = await ask_json(
        "decompose",
        roles.config("codegen-decomposer"),
        f"Specification:\n{spec}\n\nUse at most {args.max_units} units. JSON shape:\n"
        '{"units": [{"id": "short-slug", "goal": "...", "files": ["relative/path"], '
        '"depends_on": ["other-id"], "acceptance": "..."}]}' + lessons.prompt_block(),
        ledger,
        validate_units(args.max_units),
    )
    log(f"generating {len(units)} unit(s): {', '.join(unit['id'] for unit in units)}")
    for task, role in (
        ("architecture", "codegen-architect"),
        ("gen-*", "codegen-generator"),
        ("review", "review"),
    ):
        log(f"  [{task}] {roles.describe(role)}")

    workflow = build_workflow(roles, spec, units, lessons.prompt_block(), args.jobs)
    result = await workflow.run_async()
    ledger.add_workflow(result)
    statuses = {name: task.status.value for name, task in result.items()}
    for name, task in result.items():
        if not task.ok:
            blocked = f" by {', '.join(task.blocked_by)}" if task.blocked_by else ""
            log(f"  [{name}] {task.status.value}{blocked}: {task.error or ''}")
    review = text_of(result["review"].output) if result["review"].ok else ""

    attempts: list[dict[str, Any]] = []
    passed = result.ok
    while args.verify:
        passed, output = await asyncio.to_thread(
            run_verify, args.verify, workspace, args.verify_timeout
        )
        attempts.append({"passed": passed, "output_tail": output[-1500:]})
        log(f"verify attempt {len(attempts)}: {'passed' if passed else 'FAILED'}")
        if passed or len(attempts) > args.max_repairs:
            break
        # The first repair runs on the fixer's usual tier; a failure that
        # survives it is a critical path and gets the strongest model.
        tier = "critical" if len(attempts) > 1 else None
        await ask(
            f"fix-{len(attempts)}",
            roles.config("codegen-fixer", mode="auto", tier=tier),
            f"Verification command: {args.verify}\n\nIts output:\n{output}\n\n"
            f"Specification:\n{spec}\n\nReviewer findings:\n{review or '(none)'}",
            ledger,
            allow=True,
        )

    learned = await ask_json(
        "retro",
        roles.config("codegen-retro"),
        "Run record:\n"
        + json.dumps(
            {
                "units": units,
                "task_status": statuses,
                "review": review[:4000],
                "verify_command": args.verify,
                "verify_attempts": attempts,
            },
            indent=2,
        )
        + f"\n\nLessons already recorded:\n{json.dumps(recalled[-20:], indent=2)}\n\n"
        'JSON shape: {"lessons": ["..."]}',
        ledger,
        validate_lessons,
    )
    added = lessons.append(learned)

    if review:
        print(review)
    verdict = (
        f"verification {'passed' if passed else 'FAILED'} after {len(attempts)} attempt(s)"
        if args.verify
        else "no --verify command given; nothing was verified"
    )
    log(f"{verdict}; learned {len(added)} new lesson(s); usage: {ledger}")
    return 0 if passed else 1


if __name__ == "__main__":
    run(main)
