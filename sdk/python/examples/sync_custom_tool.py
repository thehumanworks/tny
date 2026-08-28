"""Register a synchronous in-process tool without decoding borrowed bytes."""

import os

import tny


def invoke(arguments_json: bytes) -> tny.ToolResult:
    # Parse only if the application wants JSON. The SDK preserves exact bytes.
    print("tool arguments:", arguments_json)
    return tny.ToolResult(b'{"source":"python-host"}')


with tny.Runtime(
    tny.RuntimeConfig(
        workspace=".",
        base_url="https://api.openai.com/v1",
        api_key=os.environ["OPENAI_API_KEY"],
        permission_mode=tny.PermissionMode.ASK,
    )
) as runtime:
    registration = runtime.register_tool(
        tny.CustomTool(
            name=b"host_context",
            description=b"Return application-owned context.",
            input_schema_json=b'{"type":"object","properties":{}}',
            handler=invoke,
            sensitivity=tny.ToolSensitivity.SENSITIVE,
        )
    )
    with runtime.create_session() as session:
        for event in session.run(b"Use host_context"):
            if isinstance(event, tny.PermissionRequestEvent):
                session.respond_permission(event, tny.PermissionDecision.ALLOW)
    registration.close()
