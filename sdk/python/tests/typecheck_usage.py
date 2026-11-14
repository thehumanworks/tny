"""Public import surface checked with mypy --strict."""

from __future__ import annotations

from pathlib import Path

import tny


def image_detail_narrowing(detail: tny.ImageDetail) -> None:
    if detail.committed:
        _retained: tny.RetainedImageDetail = detail
        _path: Path = detail.path
        _bytes: int = detail.byte_count
    else:
        _strict: tny.ImageFailureDetail = detail
        _no_path: None = detail.path


def toolkit_sync(toolkit: tny.Toolkit) -> bytes:
    image: tny.ImageResult = toolkit.generate_image("tree", output_file="tree.png")
    toolkit.edit_image("blue", images=[image.path], output_file="blue.png")
    prompt: tny.OptimisationResult = toolkit.optimize(b"Fix the parser")
    return prompt.text


async def toolkit_async(toolkit: tny.AsyncToolkit) -> bytes:
    await toolkit.generate_image("tree", output_file="tree.png")
    await toolkit.edit_image("blue", images=["tree.png"], output_file="blue.png")
    return (await toolkit.optimise("Fix the parser")).text


def toolkit_tools(toolkit: tny.Toolkit) -> str:
    specs: tuple[tny.ToolSpec, ...] = toolkit.list_tools()
    result: tny.ToolCallResult = toolkit.call_tool(
        specs[0].name, {"path": "README.md"}, permission_mode="ask"
    )
    return result.text if result.ok else ""


async def toolkit_tools_async(toolkit: tny.AsyncToolkit) -> bool:
    await toolkit.list_tools()
    return (await toolkit.call_tool("read_file", {"path": "README.md"})).ok


native_provider: tny.ProviderName = "openai"
native_config = tny.RuntimeConfig(
    workspace=".",
    state_dir=".",
    provider=native_provider,
    model="http-model",
    api_key="http-key",
)

TASK_CONFIG = tny.RuntimeConfig(workspace=".", task_preset=tny.TaskPreset("review"))
EFFORT_CONFIG = tny.RuntimeConfig(workspace=".", model="m", reasoning_effort="high")


def host_clock() -> int:
    return 1


def sync_tool(arguments: bytes) -> tny.ToolResult:
    return tny.ToolResult(arguments, is_error=False)


async def async_tool(arguments: bytes) -> bytes:
    return arguments


def consume(runtime: tny.Runtime, session: tny.Session, event: tny.AnyEvent) -> bytes:
    config: tny.RuntimeConfig = runtime.config
    if isinstance(event, tny.TextDeltaEvent):
        return event.text
    if isinstance(event, tny.UnknownEvent):
        value = event.payload.get("text", b"")
        return value if isinstance(value, bytes) else b""
    token = tny.CancellationToken()
    token.cancel()
    _ = config
    _ = session
    return b""


def callback_types(config: tny.RuntimeConfig) -> None:
    services = tny.HostServices(monotonic_ms=host_clock)
    runtime = tny.Runtime(config, host_services=services)
    registration: tny.ToolRegistration = runtime.register_tool(
        tny.CustomTool(
            name=b"host_echo",
            description=b"echo",
            input_schema_json=b'{"type":"object"}',
            handler=sync_tool,
        )
    )
    _: int = runtime.host_monotonic_ms()
    registration.close()
    runtime.close()


async def async_callback_types(config: tny.RuntimeConfig) -> None:
    runtime = tny.AsyncRuntime(config, host_services=tny.HostServices())
    registration: tny.AsyncToolRegistration = await runtime.register_tool(
        tny.AsyncCustomTool(
            name=b"host_async",
            description=b"async",
            input_schema_json=b'{"type":"object"}',
            handler=async_tool,
        )
    )
    await registration.close()
    await runtime.close()


async def workflow_types(config: tny.RuntimeConfig) -> None:
    workflow = tny.Workflow(config, max_concurrency=2)
    workflow.task("first", "inspect")
    workflow.task(
        "second",
        b"implement",
        depends_on=(
            tny.WorkflowDependency("first", context="artifact", offset=0, length=0),
        ),
    )
    result: tny.WorkflowResult = await workflow.run_async()
    task: tny.WorkflowTaskResult = result["second"]
    output: bytes = result.output("second")
    status: tny.WorkflowTaskStatus = task.status
    artifact: tny.WorkflowArtifact = task.artifact
    chunk: bytes = artifact.read(0, 0, maximum_bytes=64)
    usage: tny.UsageEvent | None = task.usage
    _ = output, status, chunk, usage, result.usage


def workflow_accounting_types(workflow: tny.Workflow, usage: tny.UsageEvent) -> None:
    total: int | float | None = workflow.partial_usage["input_tokens"]
    workflow.report_usage("task", usage)
    _ = total


async def session_accounting_types(session: tny.AsyncSession) -> None:
    snapshot: tny.UsageEvent | None = session.last_usage
    _ = snapshot


# Python tool extensions: decorators keep the decorated function's own type,
# so --strict reports "Returning Any" if a decorator ever erases it.
@tny.tool
def ext_forecast(city: str, days: int = 1) -> str:
    """Forecast for a city."""
    return f"{city}: {days}"


@tny.tool(name="alerts", timeout=5)
def ext_alerts(city: str) -> list[str]:
    return [city]


ext_projects = tny.group("projects")


@ext_projects.tool
def ext_list(status: str = "open") -> list[str]:
    return [status]


@ext_projects.tool(description="Archive one project.")
def ext_archive(key: str) -> bool:
    if not key:
        raise tny.ToolError("key is required")
    return True


def extension_tools_keep_signatures() -> str:
    alerts: list[str] = ext_alerts("Lisbon")
    archived: bool = ext_archive("TNY")
    assert archived
    return ext_forecast("Lisbon", days=2) + ext_list()[0] + alerts[0]
