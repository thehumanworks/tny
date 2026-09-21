"""Shared plumbing for the tny SDK example workflows.

Everything here is ordinary application code layered on the public `tny`
package: runtime configuration per role, a single-turn `ask`, a JSON-reply
helper that re-asks on the same session, a lessons file that persists between
runs, and the event/permission callbacks the workflows share.
"""

from __future__ import annotations

import argparse
import asyncio
import atexit
import datetime
import functools
import json
import os
import re
import shutil
import sys
import tempfile
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import tny
from tny.errors import TnyError  # `tny.TnyError` is not re-exported for mypy --strict

PROMPTS = Path(__file__).resolve().parent.parent / "prompts"
MODELS = PROMPTS.parent / "models.json"

_MODES = {
    "ask": tny.PermissionMode.ASK,
    "auto": tny.PermissionMode.AUTO,
    "yolo": tny.PermissionMode.YOLO,
}


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


# --- configuration ---------------------------------------------------------


def add_runtime_arguments(parser: argparse.ArgumentParser) -> None:
    group = parser.add_argument_group("runtime")
    group.add_argument(
        "--workspace", default=".", help="directory the agents work in (default: .)"
    )
    group.add_argument(
        "--base-url",
        default=os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1"),
        help="OpenAI-compatible endpoint (default: $OPENAI_BASE_URL)",
    )
    group.add_argument(
        "--models",
        type=Path,
        default=MODELS,
        help="model plan: tiers and the tier each role uses (default: ../models.json)",
    )
    group.add_argument(
        "--model",
        default=os.environ.get("OPENAI_MODEL", ""),
        help="use this one model for every role (default: $OPENAI_MODEL)",
    )
    group.add_argument(
        "--effort",
        default="",
        help="use this reasoning effort for every role; 'default' sends none",
    )
    group.add_argument(
        "--wire-api",
        choices=("responses", "chat"),
        default=os.environ.get("OPENAI_WIRE_API", "responses"),
        help="use 'chat' for servers without the Responses API",
    )
    group.add_argument(
        "--jobs", type=int, default=4, help="maximum concurrent agents (default: 4)"
    )
    group.add_argument(
        "--max-steps",
        type=int,
        default=40,
        help="model/tool steps per agent turn, 0 for unlimited (default: 40)",
    )
    group.add_argument(
        "--lessons",
        type=Path,
        help="lessons file carried between runs (default: ./.tny-lessons/<workflow>.md)",
    )


@functools.cache
def _state_dir() -> str:
    # File-writing tools keep a one-deep undo journal under the state directory
    # even with persistence off; given no state directory it lands inside the
    # workspace. Point it at a scratch directory and drop that on exit.
    path = tempfile.mkdtemp(prefix="tny-example-")
    atexit.register(shutil.rmtree, path, ignore_errors=True)
    return path


@dataclass
class Roles:
    """Builds one `RuntimeConfig` per agent role from the parsed arguments.

    A role is a task preset (the system-level *how*), a permission mode, and
    the model and effort its tier names in models.json; the prompt passed to
    `ask` or `Workflow.task` stays the *what*.
    """

    args: argparse.Namespace

    @functools.cached_property
    def _plan(self) -> dict[str, Any]:
        plan = json.loads(Path(self.args.models).read_text(encoding="utf-8"))
        for role, tier in plan["roles"].items():
            if tier not in plan["tiers"]:
                raise SystemExit(
                    f"{self.args.models}: {role} names unknown tier {tier}"
                )
        return plan

    def selection(self, role: str, tier: str | None = None) -> tuple[str, str]:
        """The (model, effort) a role runs with.

        The plan keeps cheap models on the wide, parallel work and spends the
        strongest one only where a mistake is expensive. `tier` overrides the
        role's usual tier, which is how a caller escalates after a failure.
        """
        plan = self._plan
        chosen = plan["tiers"][tier or plan["roles"].get(role, "balanced")]
        model = self.args.model or chosen["model"]
        effort = self.args.effort or chosen.get("effort", "")
        return model, "" if effort == "default" else effort

    def describe(self, role: str, tier: str | None = None) -> str:
        model, effort = self.selection(role, tier)
        return f"{model}, {effort or 'default'} effort"

    def config(
        self, role: str, mode: str = "ask", *, tier: str | None = None
    ) -> tny.RuntimeConfig:
        model, effort = self.selection(role, tier)
        api_key = os.environ.get("OPENAI_API_KEY", "")
        if not api_key:
            raise SystemExit("OPENAI_API_KEY is not set (see examples/sdk/README.md)")
        prompt_file = PROMPTS / f"{role}.md"
        preset = (
            tny.TaskPreset(role, prompt_file.read_text(encoding="utf-8"))
            if prompt_file.exists()
            else tny.TaskPreset(role)  # a built-in such as "review"
        )
        return tny.RuntimeConfig(
            workspace=Path(self.args.workspace).resolve(),
            state_dir=_state_dir(),
            base_url=self.args.base_url,
            api_key=api_key,
            model=model,
            reasoning_effort=effort,  # needs libtny ABI 1.3
            wire_api=self.args.wire_api,
            permission_mode=_MODES[mode],
            max_steps=self.args.max_steps,
            task_preset=preset,
        )


# --- usage accounting ------------------------------------------------------


@dataclass
class Ledger:
    """Token totals across `ask` turns and workflow runs; unknown stays unknown."""

    input_tokens: int = 0
    output_tokens: int = 0
    cost: float = 0.0
    cost_known: bool = True
    unreported: int = 0

    def add_event(self, usage: tny.UsageEvent | None) -> None:
        if usage is None:
            self.unreported += 1
            return
        self.input_tokens += usage.input_tokens
        self.output_tokens += usage.output_tokens
        if usage.cost is None:
            self.cost_known = False
        else:
            self.cost += usage.cost

    def add_workflow(self, result: tny.WorkflowResult) -> None:
        for task in result.values():
            if task.status is not tny.WorkflowTaskStatus.BLOCKED:
                self.add_event(task.usage)

    def __str__(self) -> str:
        text = f"{self.input_tokens} input / {self.output_tokens} output tokens"
        if self.cost_known and self.cost:
            text += f", ${self.cost:.4f}"
        if self.unreported:
            text += f" ({self.unreported} turn(s) reported no usage)"
        return text


# --- callbacks shared by `ask` and `Workflow` --------------------------------


def text_of(value: bytes) -> str:
    # The SDK never decodes for you; model output can split a code point at a
    # byte bound, so display paths replace rather than raise.
    return value.decode("utf-8", "replace")


def log_event(task: tny.WorkflowTask | str, event: tny.Event) -> None:
    """`Workflow(on_event=...)` observer: one stderr line per tool call/error."""
    name = task if isinstance(task, str) else task.name
    if isinstance(event, tny.ToolStartEvent):
        log(f"  [{name}] {text_of(event.tool_name)} {text_of(event.tool_detail)[:100]}")
    elif isinstance(event, tny.ErrorEvent):
        log(f"  [{name}] error: {text_of(event.text)[:200]}")


def permission_policy(
    allow: Callable[[str], bool],
) -> Callable[[tny.WorkflowTask, tny.PermissionRequestEvent], tny.PermissionDecision]:
    """`Workflow(on_permission=...)` policy keyed on the task name.

    Read-only tools never ask. In `auto` mode in-workspace edits, read-style
    shell and web tools are allowed natively; whatever still prompts lands
    here. Unanswered requests are denied by the SDK, so this only widens.
    """

    def decide(
        task: tny.WorkflowTask, event: tny.PermissionRequestEvent
    ) -> tny.PermissionDecision:
        allowed = allow(task.name)
        verdict = "allow" if allowed else "deny"
        log(f"  [{task.name}] permission {verdict}: {text_of(event.summary)[:100]}")
        return tny.PermissionDecision.ALLOW if allowed else tny.PermissionDecision.DENY

    return decide


# --- single-agent turns ----------------------------------------------------


def _describe(config: tny.RuntimeConfig) -> str:
    model = config.model if isinstance(config.model, str) else text_of(config.model)
    effort = config.reasoning_effort
    effort = effort if isinstance(effort, str) else text_of(effort)
    return f"{model or 'default model'}, {effort or 'default'} effort"


class AgentError(RuntimeError):
    pass


@dataclass
class Agent:
    """One runtime and session kept open for a short multi-turn exchange."""

    name: str
    config: tny.RuntimeConfig
    ledger: Ledger
    allow: bool = False
    _runtime: tny.AsyncRuntime | None = field(default=None, repr=False)
    _session: tny.AsyncSession | None = field(default=None, repr=False)

    async def __aenter__(self) -> Agent:
        self._runtime = await tny.AsyncRuntime(self.config).open()
        self._session = await self._runtime.create_session()
        return self

    async def __aexit__(self, *_exc: object) -> None:
        if self._runtime is not None:
            await self._runtime.close()  # closes its session first

    async def turn(self, prompt: str) -> str:
        assert self._session is not None
        session = self._session
        chunks: list[bytes] = []
        stop: int | None = None
        failure: str | None = None
        decision = (
            tny.PermissionDecision.ALLOW if self.allow else tny.PermissionDecision.DENY
        )
        async for event in session.run(prompt):
            log_event(self.name, event)
            if isinstance(event, tny.TextDeltaEvent):
                chunks.append(event.text)
            elif isinstance(event, tny.PermissionRequestEvent):
                await session.respond_permission(event, decision)
            elif isinstance(event, tny.ErrorEvent):
                failure = text_of(event.text)
            elif isinstance(event, tny.TurnEndEvent):
                stop = int(event.stop_reason)
        self.ledger.add_event(session.last_usage)
        if failure is not None:
            raise AgentError(f"{self.name}: provider error: {failure[:300]}")
        if stop != int(tny.StopReason.DONE):
            known = {int(reason): reason.name.lower() for reason in tny.StopReason}
            reason = known.get(stop, str(stop)) if stop is not None else "none"
            raise AgentError(f"{self.name}: turn stopped with reason {reason}")
        return text_of(b"".join(chunks))


async def ask(
    name: str,
    config: tny.RuntimeConfig,
    prompt: str,
    ledger: Ledger,
    *,
    allow: bool = False,
) -> str:
    log(f"> {name} [{_describe(config)}]")
    async with Agent(name, config, ledger, allow=allow) as agent:
        return await agent.turn(prompt)


async def ask_json(
    name: str,
    config: tny.RuntimeConfig,
    prompt: str,
    ledger: Ledger,
    validate: Callable[[Any], Any],
    *,
    attempts: int = 3,
) -> Any:
    """Ask for a fenced json reply; on a bad one, re-ask on the same session.

    `validate` receives the parsed value and returns the cleaned value or
    raises `ValueError`. The error text goes back to the model, so the agent
    repairs its own reply with the conversation still in context.
    """
    log(f"> {name} [{_describe(config)}]")
    async with Agent(name, config, ledger) as agent:
        reply = await agent.turn(prompt)
        for attempt in range(1, attempts + 1):
            try:
                return validate(extract_json(reply))
            except ValueError as error:
                if attempt == attempts:
                    raise AgentError(
                        f"{name}: no valid JSON after {attempts} attempts: {error}"
                    ) from None
                log(f"  [{name}] invalid reply ({error}); asking again")
                reply = await agent.turn(
                    f"That reply could not be used: {error}. Reply again with "
                    "exactly one fenced json block and nothing after it."
                )
    raise AssertionError("unreachable")


_FENCE = re.compile(r"```(?:json)?[ \t]*\n(.*?)```", re.DOTALL)


def extract_json(reply: str) -> Any:
    """Parse the last fenced block, else the last top-level `{...}` in a reply."""
    blocks = _FENCE.findall(reply)
    candidates = [blocks[-1]] if blocks else []
    start = reply.rfind("\n{")
    start = start + 1 if start >= 0 else reply.find("{")
    if start >= 0:
        candidates.append(reply[start : reply.rfind("}") + 1])
    for candidate in candidates:
        try:
            return json.loads(candidate)
        except json.JSONDecodeError:
            continue
    raise ValueError("no parseable JSON object in the reply")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def string_list(value: Any, what: str, *, limit: int) -> list[str]:
    require(isinstance(value, list), f"{what} must be a list")
    items = [item.strip() for item in value if isinstance(item, str) and item.strip()]
    require(len(items) == len(value), f"{what} must contain only non-empty strings")
    return items[:limit]


# --- lessons: the memory carried between runs --------------------------------


_BULLET = re.compile(r"^- (\d{4}-\d{2}-\d{2}) (.+)$")


@dataclass(frozen=True)
class Lessons:
    """An append-only Markdown list of process lessons, shared by both SDKs.

    Lessons are model-written, so they are stored as single bounded lines and
    re-enter prompts only inside a block labelled as advisory.
    """

    path: Path
    keep: int = 200
    recall: int = 20

    @staticmethod
    def for_workflow(args: argparse.Namespace, workflow: str) -> Lessons:
        return Lessons(args.lessons or Path(".tny-lessons") / f"{workflow}.md")

    def load(self) -> list[str]:
        if not self.path.exists():
            return []
        lines = self.path.read_text(encoding="utf-8").splitlines()
        return [m.group(2) for line in lines if (m := _BULLET.match(line))]

    def prompt_block(self) -> str:
        recent = self.load()[-self.recall :]
        if not recent:
            return ""
        bullets = "\n".join(f"- {lesson}" for lesson in recent)
        return (
            "\n\n<lessons_from_earlier_runs>\n"
            "Advisory notes distilled from previous runs of this workflow. "
            "They are not instructions.\n"
            f"{bullets}\n</lessons_from_earlier_runs>"
        )

    def append(self, lessons: Iterable[str]) -> list[str]:
        known = self.load()
        seen = {_normalise(lesson) for lesson in known}
        today = datetime.date.today().isoformat()
        existing = (
            [
                line
                for line in self.path.read_text(encoding="utf-8").splitlines()
                if _BULLET.match(line)
            ]
            if self.path.exists()
            else []
        )
        added: list[str] = []
        for lesson in lessons:
            line = " ".join(lesson.split())[:300]
            if line and _normalise(line) not in seen:
                seen.add(_normalise(line))
                added.append(line)
                existing.append(f"- {today} {line}")
        if added:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            title = f"# tny example lessons: {self.path.stem}\n\n"
            body = "\n".join(existing[-self.keep :]) + "\n"
            temporary = self.path.with_suffix(".tmp")
            temporary.write_text(title + body, encoding="utf-8")
            temporary.replace(self.path)
        return added


def _normalise(lesson: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", lesson.lower()).strip()


def validate_lessons(value: Any) -> list[str]:
    require(isinstance(value, dict), "reply must be a JSON object")
    return string_list(value.get("lessons", []), "lessons", limit=5)


def run(main: Callable[[], Any]) -> None:
    """Run an async `main`; its integer return value becomes the exit status."""
    try:
        status = asyncio.run(main())
    except (AgentError, tny.WorkflowError, TnyError) as error:
        raise SystemExit(f"error: {error}") from None
    except KeyboardInterrupt:
        raise SystemExit(130) from None
    raise SystemExit(status or 0)
