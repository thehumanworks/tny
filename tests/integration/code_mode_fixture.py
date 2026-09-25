"""Explicit run_code calls for mock providers; never a product fallback."""

import json


def lua_string(value):
    """Quote UTF-8 fixture data without JSON-only Lua escape sequences."""
    level = 0
    while "]" + "=" * level + "]" in value:
        level += 1
    return "[" + "=" * level + "[" + value + "]" + "=" * level + "]"


def code_call(name, arguments):
    if name == "run_code":
        return name, arguments
    if not isinstance(arguments, str):
        arguments = json.dumps(arguments, ensure_ascii=False)
    code = f"print(tools.call({lua_string(name)}, {lua_string(arguments)}))"
    return "run_code", json.dumps(
        {"code": code, "timeout_ms": 30000}, ensure_ascii=False
    )


def _split_arguments(value, holders, key):
    if not holders:
        return
    width = max(1, len(value) // len(holders))
    for index, holder in enumerate(holders):
        holder[key] = (
            value[index * width :]
            if index == len(holders) - 1
            else value[index * width : (index + 1) * width]
        )


def code_chat_frames(frames):
    """Keep stream metadata/index reuse and argument fragmentation intact."""
    records = []
    active = {}
    for frame in frames:
        for choice in frame.get("choices", []):
            for call in choice.get("delta", {}).get("tool_calls", []):
                index = call.get("index", 0)
                if call.get("id") or index not in active:
                    record = {"functions": [], "arguments": []}
                    records.append(record)
                    active[index] = record
                record = active[index]
                function = call.get("function", {})
                record["functions"].append(function)
                if "name" in function:
                    record["name"] = function["name"]
                if "arguments" in function:
                    record["arguments"].append(function)
    for record in records:
        if "name" not in record:
            continue
        name, arguments = code_call(
            record["name"], "".join(f["arguments"] for f in record["arguments"])
        )
        for function in record["functions"]:
            if "name" in function:
                function["name"] = name
        _split_arguments(arguments, record["arguments"], "arguments")
    return frames


def code_response_events(events):
    """Wrap complete calls while preserving added/done and delta edge cases."""
    records = {}
    for event in events:
        item = event.get("item", {})
        if item.get("type") == "function_call":
            record = records.setdefault(
                item.get("id", item.get("call_id")), {"items": [], "deltas": []}
            )
            record["items"].append(item)
            if item.get("name"):
                record["name"] = item["name"]
        if event.get("type") == "response.function_call_arguments.delta" and event.get(
            "item_id"
        ):
            record = records.setdefault(event["item_id"], {"items": [], "deltas": []})
            if event.get("delta"):
                record["deltas"].append(event)
    for record in records.values():
        if "name" not in record:
            continue
        arguments = next(
            (
                item["arguments"]
                for item in reversed(record["items"])
                if item.get("arguments")
            ),
            None,
        )
        if arguments is None:
            arguments = "".join(event["delta"] for event in record["deltas"])
        name, wrapped = code_call(record["name"], arguments)
        for item in record["items"]:
            if "name" in item:
                item["name"] = name
            if item.get("arguments"):
                item["arguments"] = wrapped
        _split_arguments(wrapped, record["deltas"], "delta")
        identifiers = {item.get("id", item.get("call_id")) for item in record["items"]}
        for event in events:
            if (
                event.get("type") == "response.function_call_arguments.done"
                and event.get("item_id") in identifiers
            ):
                event["arguments"] = wrapped
    for event in events:
        for item in event.get("response", {}).get("output", []):
            if item.get("type") == "function_call":
                item["name"], item["arguments"] = code_call(
                    item["name"], item["arguments"]
                )
    return events
