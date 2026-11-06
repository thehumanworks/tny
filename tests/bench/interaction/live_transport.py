#!/usr/bin/env python3
"""Opt-in SIWC SSE/WS paired probe. Never logs credentials or response text.

Run tny --provider openai status first to refresh via the normal locked store.
This isolates transport, not tny's full harness. Optional websocket-client==1.9.2.
"""

import argparse
import http.client
import json
import time
from pathlib import Path


def measure(transport, token, model, turns):
    start = time.perf_counter()
    conn = None
    rows = []
    try:
        if transport == "sse":
            conn = http.client.HTTPSConnection("api.openai.com", timeout=40)
            conn.connect()
        else:
            import websocket

            conn = websocket.create_connection(
                "wss://api.openai.com/v1/responses",
                header={"Authorization": "Bearer " + token},
                timeout=40,
            )
        connect_ms = (time.perf_counter() - start) * 1000
        history = []
        previous = None
        for turn in range(turns):
            new = [{"role": "user", "content": "Reply with exactly OK."}]
            history.extend(new)
            payload = {
                "model": model,
                "store": False,
                "reasoning": {"effort": "low"},
                "input": new if previous and transport == "websocket" else history,
            }
            start = time.perf_counter()
            if transport == "sse":
                payload["stream"] = True
                conn.request(
                    "POST",
                    "/v1/responses",
                    json.dumps(payload),
                    {
                        "Authorization": "Bearer " + token,
                        "Content-Type": "application/json",
                    },
                )
                response = conn.getresponse()
                if response.status != 200:
                    return {
                        "transport": transport,
                        "status": response.status,
                        "runs": rows,
                    }

                def stream():
                    for line in response:
                        if (
                            line.startswith(b"data: ")
                            and line.strip() != b"data: [DONE]"
                        ):
                            yield json.loads(line[6:])

                iterator = stream()
            else:
                payload["type"] = "response.create"
                if previous:
                    payload["previous_response_id"] = previous
                conn.send(json.dumps(payload))

                def stream():
                    while True:
                        yield json.loads(conn.recv())

                iterator = stream()
            request_bytes = len(json.dumps(payload).encode())
            output_items = []
            first = None
            completed = None
            for event in iterator:
                kind = event.get("type", "")
                if kind == "response.output_item.done":
                    output_items.append(event["item"])
                if kind == "response.output_text.delta" and first is None:
                    first = time.perf_counter()
                if kind in ("error", "response.failed", "response.incomplete"):
                    return {"transport": transport, "error_type": kind, "runs": rows}
                if kind == "response.completed":
                    completed = event["response"]
                    break
            if not completed or first is None:
                raise RuntimeError("missing text/completion")
            end = time.perf_counter()
            if transport == "sse":
                response.read()  # fully consume framing before reusing connection
            previous = completed.get("id")
            history.extend(completed.get("output") or output_items)
            if not output_items and not completed.get("output"):
                raise RuntimeError("missing replayable output")
            rows.append(
                {
                    "turn": turn,
                    "connect_ms": connect_ms if turn == 0 else 0,
                    "ttft_ms": (first - start) * 1000,
                    "complete_ms": (end - start) * 1000,
                    "request_bytes": request_bytes,
                    "input_tokens": completed.get("usage", {}).get("input_tokens"),
                }
            )
        return {"transport": transport, "runs": rows}
    except Exception as exc:
        # Exception strings can contain headers, URLs or server payloads.
        return {"transport": transport, "error_class": type(exc).__name__, "runs": rows}
    finally:
        if conn:
            conn.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--live", action="store_true", required=True)
    p.add_argument("--model", default="gpt-6.1-sol")
    p.add_argument("--pairs", type=int, default=3)
    p.add_argument("--turns", type=int, default=3)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    auth = json.loads((Path.home() / ".tny/openai-auth.json").read_text())
    if not auth.get("plan_usage") or not auth.get("access_token"):
        raise SystemExit("Run tny --provider openai login first")
    result = {
        "model": args.model,
        "probe": "direct SIWC transport, not full harness",
        "pairs": [],
    }
    for i in range(args.pairs):
        pair = []
        for transport in ("sse", "websocket") if i % 2 == 0 else ("websocket", "sse"):
            row = measure(transport, auth["access_token"], args.model, args.turns)
            pair.append(row)
            print(json.dumps(row), flush=True)
        result["pairs"].append(pair)
        args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
