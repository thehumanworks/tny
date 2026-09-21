#!/usr/bin/env python3
"""Auto-research: a research loop that improves within a run and across runs.

    plan ──► investigate (parallel) ──► synthesise ──► critique ─┐
      ▲            ▲                                             │
      │            └──── gaps become the next round's questions ─┘
      └── lessons from earlier runs          retro ──► lessons file

Within a run the critic's gaps drive further rounds until the score reaches
`--target`. Across runs a retrospective distils process lessons into a file
that the next run's planner reads. The Python and TypeScript versions share
that file and the role prompts in ../prompts.

    OPENAI_API_KEY=... python3 auto_research.py "How does session isolation work?"
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any

import tny
from _common import (
    AgentError,
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
    parser.add_argument("topic", help="what to research")
    parser.add_argument(
        "--rounds", type=int, default=3, help="maximum rounds (default: 3)"
    )
    parser.add_argument(
        "--target",
        type=float,
        default=0.8,
        help="stop once the critic scores the report at least this (default: 0.8)",
    )
    parser.add_argument(
        "--questions", type=int, default=4, help="questions per round (default: 4)"
    )
    parser.add_argument(
        "--out", type=Path, help="write the report here instead of stdout"
    )
    add_runtime_arguments(parser)
    return parser.parse_args()


def validate_plan(limit: int) -> Any:
    def validate(value: Any) -> list[dict[str, str]]:
        require(isinstance(value, dict), "reply must be a JSON object")
        questions = value.get("questions")
        require(
            isinstance(questions, list) and bool(questions),
            "questions must be a non-empty list",
        )
        cleaned: list[dict[str, str]] = []
        for item in questions[:limit]:
            require(isinstance(item, dict), "each question must be an object")
            identifier, question = item.get("id"), item.get("question")
            require(
                isinstance(identifier, str) and bool(_ID.match(identifier)),
                "each id must be a short slug of letters, digits, '-' or '_'",
            )
            require(
                isinstance(question, str) and bool(question.strip()),
                "question must be text",
            )
            require(
                all(identifier != seen["id"] for seen in cleaned), "ids must be unique"
            )
            approach = item.get("approach")
            cleaned.append(
                {
                    "id": identifier,
                    "question": question.strip(),
                    "approach": approach.strip() if isinstance(approach, str) else "",
                }
            )
        return cleaned

    return validate


def validate_critique(value: Any) -> dict[str, Any]:
    require(isinstance(value, dict), "reply must be a JSON object")
    score = value.get("score")
    require(
        isinstance(score, (int, float))
        and not isinstance(score, bool)
        and 0 <= score <= 1,
        "score must be a number from 0 to 1",
    )
    return {
        "score": float(score),
        "gaps": string_list(value.get("gaps", []), "gaps", limit=8),
    }


async def investigate(
    roles: Roles,
    topic: str,
    questions: list[dict[str, str]],
    ledger: Ledger,
    jobs: int,
) -> dict[str, str]:
    """Fan out one agent per question; a failed branch never sinks the others."""
    workflow = tny.Workflow(
        # `auto` lets investigators read, run read-style shell and use the
        # web; anything else prompts, and the policy below denies it.
        roles.config("research-investigator", mode="auto"),
        max_concurrency=jobs,
        on_event=log_event,
        on_permission=permission_policy(lambda _task: False),
    )
    for item in questions:
        approach = (
            f"\nSuggested approach: {item['approach']}" if item["approach"] else ""
        )
        workflow.task(
            f"q-{item['id']}",
            f"Research topic: {topic}\n\nYour question: {item['question']}{approach}",
        )
    result = await workflow.run_async()
    ledger.add_workflow(result)

    findings: dict[str, str] = {}
    for item in questions:
        task = result[f"q-{item['id']}"]
        if task.ok:
            findings[item["question"]] = text_of(task.output)
        else:
            log(f"  [{task.name}] {task.status.value}: {task.error}")
    if not findings:
        raise AgentError("every investigator failed; nothing to synthesise")
    return findings


async def main() -> None:
    args = parse_arguments()
    roles, ledger = Roles(args), Ledger()
    lessons = Lessons.for_workflow(args, "auto-research")
    recalled = lessons.load()
    log(f"recalled {len(recalled)} lesson(s) from {lessons.path}")

    questions = await ask_json(
        "plan",
        roles.config("research-planner"),
        f"Research topic: {args.topic}\n\n"
        f"Plan at most {args.questions} questions. JSON shape:\n"
        '{"questions": [{"id": "short-slug", "question": "...", "approach": "..."}]}'
        + lessons.prompt_block(),
        ledger,
        validate_plan(args.questions),
    )

    report = ""
    history: list[dict[str, Any]] = []
    for round_number in range(1, args.rounds + 1):
        log(
            f"round {round_number}/{args.rounds}: investigating {len(questions)} "
            f"question(s) [{roles.describe('research-investigator')}]"
        )
        findings = await investigate(roles, args.topic, questions, ledger, args.jobs)
        sections = "\n\n".join(
            f'<finding question="{question}">\n{finding}\n</finding>'
            for question, finding in findings.items()
        )
        previous = f"\n\nPrevious draft to revise:\n{report}" if report else ""
        report = await ask(
            "synthesise",
            roles.config("research-synthesiser"),
            f"Research topic: {args.topic}\n\nNew findings:\n{sections}{previous}",
            ledger,
        )
        critique = await ask_json(
            "critique",
            roles.config("research-critic"),
            f"Round {round_number} of {args.rounds}. Research topic: {args.topic}\n\n"
            f"Report:\n{report}\n\n"
            'JSON shape: {"score": 0.0-1.0, "strengths": ["..."], "gaps": ["one '
            'answerable follow-up question per gap"]}',
            ledger,
            validate_critique,
        )
        history.append(
            {
                "round": round_number,
                "questions": [item["question"] for item in questions],
                "answered": len(findings),
                "score": critique["score"],
                "gaps": critique["gaps"],
            }
        )
        log(f"  score {critique['score']:.2f}, {len(critique['gaps'])} gap(s)")
        if critique["score"] >= args.target or not critique["gaps"]:
            break
        questions = [
            {"id": f"r{round_number + 1}-{index}", "question": gap, "approach": ""}
            for index, gap in enumerate(critique["gaps"][: args.questions], start=1)
        ]

    learned = await ask_json(
        "retro",
        roles.config("research-retro"),
        f"Research topic: {args.topic}\n\nRun history:\n{json.dumps(history, indent=2)}"
        f"\n\nLessons already recorded:\n{json.dumps(recalled[-20:], indent=2)}\n\n"
        'JSON shape: {"lessons": ["..."]}',
        ledger,
        validate_lessons,
    )
    added = lessons.append(learned)

    if args.out:
        args.out.write_text(report + "\n", encoding="utf-8")
        log(f"report written to {args.out}")
    else:
        print(report)
    scores = " -> ".join(f"{entry['score']:.2f}" for entry in history)
    log(
        f"scores by round: {scores}; learned {len(added)} new lesson(s); usage: {ledger}"
    )


if __name__ == "__main__":
    run(main)
