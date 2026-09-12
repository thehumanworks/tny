#!/usr/bin/env python3
"""Stable SUBAGENT_* diagnostics end to end (docs/adr/0087).

Real TNY_TOOLS=all parents and real `tny ask` children run against the
localhost fixture shared with test_subagent.py. Every documented code a live
turn can reach is asserted as an exact string: INVALID_ARGUMENT,
UNSUPPORTED_ACTION, UNSUPPORTED_CONTEXT (ephemeral parent, shell tool
profile), SESSION_NOT_FOUND, SESSION_BUSY (a live child and the parent's own
id), CHILD_FAILED and CANCELLED. Validation and unsupported-action rejections
start no child and emit no subagent lifecycle event; missing and busy ids start
no child and create or change no session. A child whose
provider echoes the dummy key and a secret-bearing gateway URL fails without
those sentinels reaching the tool result, the parent's session files,
extension events or any child argv.

AUTH_UNAVAILABLE (the parent itself could not have connected), LAUNCH_FAILED
(the child is this very executable) and INVALID_RESPONSE (a real tny child
always prints its --json result) cannot be produced by a live parent turn;
tests/test_core.c asserts their exact strings directly.
"""

import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_subagent as fx  # noqa: E402
from test_subagent import Fail, check  # noqa: E402

CREATE_EXAMPLE = fx.CREATE_EXAMPLE
MESSAGE_EXAMPLE = '{"action":"message","id":"<id from create>","prompt":"..."}'
SECRET_KEY = "dummy-sk-SENTINELKEY-4d1"
SECRET_PATH = "/gw-SENTINELGW-88a/v1"
ECHO = "SENTINELECHO"
SENTINELS = (SECRET_KEY, "SENTINELGW", ECHO)
MISSING_ID = "0123456789abcdef"

E_ACTION = (
    "error: SUBAGENT_INVALID_ARGUMENT: action must be create, message, inspect or "
    f"lifecycle. Example: {CREATE_EXAMPLE}"
)
E_UNSUPPORTED = (
    "error: SUBAGENT_UNSUPPORTED_ACTION: supported actions are create, message, "
    "inspect and lifecycle; relationship, configure and queued messages are not "
    f"supported. Example: {CREATE_EXAMPLE}"
)
E_FIELDS = (
    "error: SUBAGENT_INVALID_ARGUMENT: only action, id and prompt are accepted. "
    f"Example: {CREATE_EXAMPLE}"
)
E_PROMPT = (
    "error: SUBAGENT_INVALID_ARGUMENT: create needs a nonempty UTF-8 prompt. "
    f"Example: {CREATE_EXAMPLE}"
)
E_MESSAGE_ID = (
    "error: SUBAGENT_INVALID_ARGUMENT: message needs the 16-character lowercase hex "
    f"id returned by create. Example: {MESSAGE_EXAMPLE}"
)
E_NO_PROMPT = (
    "error: SUBAGENT_INVALID_ARGUMENT: inspect takes no prompt. Example: "
    '{"action":"inspect","id":"<id from create>"}'
)
E_NOT_FOUND = (
    "error: SUBAGENT_SESSION_NOT_FOUND: no stored child session has that id in "
    f"this workspace; create one with {CREATE_EXAMPLE} and use the id it returns"
)
E_SELF = (
    "error: SUBAGENT_SESSION_BUSY: that id is this parent session, which is "
    "running this turn; message only ids returned by create"
)
E_PROFILE = (
    "error: SUBAGENT_UNSUPPORTED_CONTEXT: subagent is unavailable in the terminal "
    'tool profile; run tny ask -B --json "..." through terminal and read it with '
    "tny session <id> --wait, or use TNY_TOOLS=all"
)
E_CANCELLED_EARLY = (
    "error: SUBAGENT_CANCELLED: the turn was cancelled and the child process was "
    "stopped before it reported a session"
)


def e_busy(sid):
    return (
        "error: SUBAGENT_SESSION_BUSY: that child is running a turn; check "
        f'{{"action":"lifecycle","id":"{sid}"}} and retry after it finishes'
    )


def e_ephemeral(action):
    return (
        "error: SUBAGENT_UNSUPPORTED_CONTEXT: ephemeral children are one-shot and "
        f"store no session, so {action} has nothing to address; use {CREATE_EXAMPLE} "
        "with the complete task, or run tny without --ephemeral"
    )


def e_child_failed(sid):
    return (
        f"error: SUBAGENT_CHILD_FAILED: child {sid} failed (exit 2); its session keeps "
        f'the details. Check {{"action":"lifecycle","id":"{sid}"}} and retry with '
        "action=message"
    )


def e_cancelled(sid):
    return (
        f"error: SUBAGENT_CANCELLED: the turn was cancelled and child {sid} was "
        f'stopped; check {{"action":"lifecycle","id":"{sid}"}} before continuing'
    )


EXTENSION = (
    "import json, os\n"
    "from tny_ext import (SubagentStartEvent, SubagentEndEvent, PostToolUseEvent,\n"
    "                     PostToolFailureEvent)\n"
    "FIELDS = ('subagent_id', 'action', 'outcome', 'ok', 'tool_name', 'result')\n"
    "def setup(api):\n"
    "    def record(event):\n"
    "        path = os.environ.get('TNY_TEST_SUBAGENT_LOG')\n"
    "        if not path: return\n"
    "        row = {'type': event.type}\n"
    "        row.update({f: getattr(event, f) for f in FIELDS if hasattr(event, f)})\n"
    "        with open(path, 'a', encoding='utf-8') as out:\n"
    "            out.write(json.dumps(row) + '\\n')\n"
    "    for kind in (SubagentStartEvent, SubagentEndEvent, PostToolUseEvent,\n"
    "                 PostToolFailureEvent):\n"
    "        api.on(kind, record)\n"
)


def events(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f]


def subagent_events(path):
    return [e for e in events(path) if e["type"] in ("subagent_start", "subagent_end")]


def tree_text(root):
    """Every byte under root, for sentinel scans of session records/logs."""
    chunks = []
    for base, _dirs, files in os.walk(root):
        for name in files:
            with open(os.path.join(base, name), "rb") as f:
                chunks.append(f.read().decode("utf-8", "replace"))
    return "\n".join(chunks)


def parent_session(home, scenario, ids):
    for sid in ids:
        doc = fx.session_doc(home, sid)
        users = [
            m.get("content", "") for m in doc.get("messages", []) if m["role"] == "user"
        ]
        if users and f"scenario:{scenario}" in users[0]:
            return sid
    raise Fail(f"{scenario}: parent session not found among {ids}")


def diag_rejections(provider, home, workspace, log):
    """Invalid, unsupported and missing-id calls: exact strings, no child, no
    new or changed session; only the runtime missing-id message is framed by
    the usual lifecycle events."""
    env = fx.base_env(home, provider, TNY_TEST_SUBAGENT_LOG=log)
    s = "rejections"
    calls = [
        (
            {"action": "create", "prompt": "child-task:r1 x", "id": "wallpaper-blue"},
            fx.E_CREATE_ID,
        ),
        (
            {"action": "create", "prompt": "child-task:r2 x", "id": MISSING_ID},
            fx.E_CREATE_ID,
        ),
        ({"action": "create", "prompt": "child-task:r3 x", "name": "x"}, E_FIELDS),
        ({"action": "create", "prompt": ""}, E_PROMPT),
        ({"action": "create", "prompt": 7}, E_PROMPT),
        ({"prompt": "child-task:r4 x"}, E_ACTION),
        ({"action": "relationship", "id": "wallpaper-blue"}, E_UNSUPPORTED),
        ({"action": "configure"}, E_UNSUPPORTED),
        (
            {"action": "message", "id": "last", "prompt": "child-task:r5 x"},
            E_MESSAGE_ID,
        ),
        ({"action": "message", "id": 42, "prompt": "child-task:r6 x"}, E_MESSAGE_ID),
        ({"action": "inspect", "id": MISSING_ID, "prompt": "x"}, E_NO_PROMPT),
        (
            {"action": "message", "id": MISSING_ID, "prompt": "child-task:r7 x"},
            E_NOT_FOUND,
        ),
        ({"action": "inspect", "id": MISSING_ID}, E_NOT_FOUND),
        ({"action": "lifecycle", "id": MISSING_ID}, E_NOT_FOUND),
    ]
    provider.plan(s, *[("subagent", args) for args, _want in calls])
    before = set(fx.session_dirs(home))
    payload = fx.run_parent(env, workspace, s)
    check(fx.statuses(payload) == [("subagent", "error")] * len(calls), payload)
    for (args, want), got in zip(calls, provider.results[s]):
        check(got == want, f"{args}: {got!r} != {want!r}")
    child = [r for r in provider.requests if r.get("role") == "child"]
    check(not child, f"a rejected call reached a child: {child}")
    # Validation/unsupported rejections happen before the tool starts, so no
    # lifecycle event; a message to a missing id is a runtime answer inside
    # the usual correlated start/end pair.
    lifecycle = [
        (e["type"], e["subagent_id"], e["action"], e.get("ok"))
        for e in subagent_events(log)
    ]
    check(
        lifecycle
        == [
            ("subagent_start", MISSING_ID, "message", None),
            ("subagent_end", MISSING_ID, "message", False),
        ],
        lifecycle,
    )
    added = set(fx.session_dirs(home)) - before
    check(len(added) == 1 and MISSING_ID not in added, f"sessions created: {added}")


def diag_contexts(provider, tmp):
    """Ephemeral parents address no stored child; shell profiles hide the
    tool and a replayed call names the fallback."""
    home = os.path.join(tmp, "ctx-home")
    workspace = os.path.join(home, "ws")
    os.makedirs(workspace)
    env = fx.base_env(home, provider)
    s = "ctx-ephemeral"
    provider.plan(
        s,
        (
            "subagent",
            {"action": "message", "id": MISSING_ID, "prompt": "child-task:e1 x"},
        ),
        ("subagent", {"action": "inspect", "id": MISSING_ID}),
        ("subagent", {"action": "lifecycle", "id": MISSING_ID}),
    )
    fx.run_parent(env, workspace, s, flags=(*fx.DEFAULT_FLAGS, "--ephemeral"))
    check(
        provider.results[s]
        == [e_ephemeral(a) for a in ("message", "inspect", "lifecycle")],
        provider.results[s],
    )
    check(
        not os.path.exists(os.path.join(home, ".tny", "sessions")), "ephemeral stored"
    )

    s = "ctx-profile"
    provider.plan(s, ("subagent", {"action": "create", "prompt": "child-task:p1 x"}))
    fx.run_parent(fx.base_env(home, provider, TNY_TOOLS="terminal"), workspace, s)
    tools = provider.parent_requests(s)[0]["tools"]
    check(
        "subagent" not in tools and "terminal" in tools,
        f"terminal profile tools: {tools}",
    )
    check(provider.results[s] == [E_PROFILE], provider.results[s])
    check(not provider.child_requests("p1"), "a hidden subagent started a child")


def diag_busy(provider, home, workspace):
    """A live child is busy: message changes nothing, lifecycle reports the
    writer lock; the parent's own id is busy too."""
    env = fx.base_env(home, provider)
    s = "busy-create"
    provider.plan(s, ("subagent", {"action": "create", "prompt": "child-task:okb0 x"}))
    fx.run_parent(env, workspace, s)
    sid = provider.created_id(s)

    provider.hold("holdb")
    s_hold = "busy-hold"
    provider.plan(
        s_hold,
        ("subagent", {"action": "message", "id": sid, "prompt": "child-task:holdb x"}),
    )
    held = {}
    worker = threading.Thread(
        target=lambda: held.update(payload=fx.run_parent(env, workspace, s_hold)),
        daemon=True,
    )
    worker.start()
    check(provider.arrived["holdb"].wait(60), "held child never reached the provider")
    before = fx.session_bytes(home, sid)
    s_busy = "busy-intruder"
    provider.plan(
        s_busy,
        (
            "subagent",
            {"action": "message", "id": sid, "prompt": "child-task:okintruder x"},
        ),
        ("subagent", {"action": "lifecycle", "id": sid}),
    )
    fx.run_parent(env, workspace, s_busy)
    check(
        fx.session_bytes(home, sid) == before, "busy message changed the child session"
    )
    check(not provider.child_requests("okintruder"), "busy message started a child")
    check(
        provider.results[s_busy]
        == [
            e_busy(sid),
            f"subagent {sid}\nstatus: running\nexit_code: null\nrunning: true\nresumable: false",
        ],
        provider.results[s_busy],
    )
    provider.holds["holdb"].set()
    worker.join(90)
    check(
        provider.results[s_hold] == [fx.success_text(sid, "CHILD-OK holdb")],
        provider.results,
    )
    check(fx.session_doc(home, sid).get("turns") == 2, "held message lost its turn")

    s_self = "busy-self"
    known = set(fx.session_dirs(home))

    def own_id():
        fresh = set(fx.session_dirs(home)) - known
        check(len(fresh) == 1, f"cannot identify the parent session: {fresh}")
        return {"action": "message", "id": fresh.pop(), "prompt": "child-task:self x"}

    provider.plan(s_self, ("subagent", own_id))
    fx.run_parent(env, workspace, s_self)
    check(provider.results[s_self] == [E_SELF], provider.results[s_self])
    check(not provider.child_requests("self"), "self message started a child")


def diag_secret_echo(provider, home, workspace, log):
    """The child's provider echoes the key and a secret-bearing gateway URL;
    only the stable CHILD_FAILED line reaches the parent."""
    env = fx.base_env(
        home,
        provider,
        OPENAI_API_KEY=SECRET_KEY,
        OPENAI_BASE_URL=provider.url(SECRET_PATH),
        TNY_DEBUG_PROVIDER_ERRORS="1",
        TNY_TEST_SUBAGENT_LOG=log,
    )
    s = "echo"
    provider.plan(
        s,
        ("subagent", {"action": "create", "prompt": "child-task:fail1 x"}),
        (
            "subagent",
            lambda: {
                "action": "message",
                "id": child_id(provider.results[s][0]),
                "prompt": "child-task:fail2 x",
            },
        ),
    )
    before = set(fx.session_dirs(home))
    payload = fx.run_parent(env, workspace, s)
    check(fx.statuses(payload) == [("subagent", "error")] * 2, payload)
    sid = child_id(provider.results[s][0])
    check(provider.results[s] == [e_child_failed(sid)] * 2, provider.results[s])
    for tag in ("fail1", "fail2"):
        req = provider.child_requests(tag)[0]
        check(req["auth"] == f"Bearer {SECRET_KEY}" and SECRET_PATH in req["path"], req)
    # The child really did hold the echo (opt-in provider detail), so the
    # parent had it available and did not relay it.
    child_doc = json.dumps(fx.session_doc(home, sid))
    check(ECHO in child_doc, "fixture echo never reached the child's own result")
    parent = parent_session(home, s, set(fx.session_dirs(home)) - before - {sid})
    surfaces = {
        "tool results": "\n".join(provider.results[s]),
        "parent stdout/stderr": json.dumps(payload),
        "parent session files": tree_text(fx.session_dirs(home)[parent]),
        "extension events": json.dumps(events(log)),
        "child argv": json.dumps(
            [r["argv"] for r in provider.requests if r.get("tag") in ("fail1", "fail2")]
        ),
    }
    for where, text in surfaces.items():
        for sentinel in SENTINELS:
            check(sentinel not in text, f"{sentinel!r} leaked into {where}")
    lifecycle = [
        (e["type"], e["action"], e.get("outcome"), e.get("ok"))
        for e in subagent_events(log)
    ]
    check(
        lifecycle
        == [
            ("subagent_start", "create", None, None),
            ("subagent_end", "create", "error", False),
            ("subagent_start", "message", None, None),
            ("subagent_end", "message", "error", False),
        ],
        lifecycle,
    )
    check(subagent_events(log)[2]["subagent_id"] == sid, subagent_events(log))

    # Without the opt-in, the child's own records hold no sentinel either.
    env.pop("TNY_DEBUG_PROVIDER_ERRORS")
    s = "echo-default"
    provider.plan(s, ("subagent", {"action": "create", "prompt": "child-task:fail3 x"}))
    fx.run_parent(env, workspace, s)
    sid = child_id(provider.results[s][0])
    check(provider.results[s] == [e_child_failed(sid)], provider.results[s])
    text = tree_text(fx.session_dirs(home)[sid])
    for sentinel in SENTINELS:
        check(sentinel not in text, f"{sentinel!r} persisted in the child's session")


def child_id(result):
    marker = "child "
    start = result.find(marker)
    check(start >= 0, f"no child id in {result!r}")
    return result[start + len(marker) : start + len(marker) + 16]


def diag_cancel(provider, home, workspace):
    """Interrupting the parent stops the owned child tree and records the
    stable CANCELLED result; the child's lifecycle is not success."""
    env = fx.base_env(home, provider)
    s = "cancel"
    provider.hold("holdcancel")
    provider.plan(
        s, ("subagent", {"action": "create", "prompt": "child-task:holdcancel x"})
    )
    before = set(fx.session_dirs(home))
    proc = subprocess.Popen(
        [fx.TNY, *fx.DEFAULT_FLAGS, "ask", "--json", f"scenario:{s} go"],
        cwd=workspace,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        check(
            provider.arrived["holdcancel"].wait(60), "child never reached the provider"
        )
        pids = [pid for pid, _args in fx.child_processes()]
        check(pids, "no live child process to cancel")
        proc.send_signal(signal.SIGINT)
        _out, err = proc.communicate(timeout=60)
    finally:
        if proc.poll() is None:
            proc.kill()
        provider.holds["holdcancel"].set()
    check(proc.returncode == 130, f"parent exit {proc.returncode}: {err}")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and any(alive(p) for p in pids):
        time.sleep(0.1)
    check(not any(alive(p) for p in pids), f"child processes survived: {pids}")
    added = set(fx.session_dirs(home)) - before
    parent = parent_session(home, s, added)
    doc = fx.session_doc(home, parent)
    tool = [
        (m["tool_call_id"], m["content"])
        for m in doc["messages"]
        if m["role"] == "tool"
    ]
    children = added - {parent}
    # exactly one result, under the model's own call id
    check(
        len(tool) == 1 and tool[0][0] == f"call_{s}_0", f"parent tool results: {tool}"
    )
    tool = [content for _cid, content in tool]
    expected = [e_cancelled(c) for c in children] + [E_CANCELLED_EARLY]
    check(tool[0] in expected, f"cancel result {tool[0]!r} not in {expected}")
    for child in children:
        s_state = f"cancel-state-{child}"
        provider.plan(s_state, ("subagent", {"action": "lifecycle", "id": child}))
        fx.run_parent(env, workspace, s_state)
        state = provider.results[s_state][0]
        check(
            state.startswith(f"subagent {child}\nstatus: ")
            and ("status: interrupted\n" in state or "status: stale\n" in state)
            and "running: false" in state,
            state,
        )


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def run():
    provider = fx.Provider(echo_marker=ECHO)
    try:
        with tempfile.TemporaryDirectory(prefix="tny-subagent-diagnostics-") as tmp:
            home, workspace = fx.make_home(tmp)
            extensions = os.path.join(home, ".tny", "extensions")
            os.makedirs(extensions)
            with open(
                os.path.join(extensions, "subagent_log.py"), "w", encoding="utf-8"
            ) as f:
                f.write(EXTENSION)
            diag_rejections(
                provider, home, workspace, os.path.join(tmp, "rejections.jsonl")
            )
            diag_contexts(provider, tmp)
            diag_busy(provider, home, workspace)
            diag_secret_echo(provider, home, workspace, os.path.join(tmp, "echo.jsonl"))
            diag_cancel(provider, home, workspace)
    finally:
        provider.close()
    print(
        "ok  subagent diagnostics: exact SUBAGENT_* codes, no child for rejected "
        "calls, busy/missing ids untouched, secret echo contained, cancel owned"
    )


if __name__ == "__main__":
    try:
        run()
    except Fail as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        sys.exit(1)
