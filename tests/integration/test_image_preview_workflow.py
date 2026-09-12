#!/usr/bin/env python3
"""Real non-job preview requests. Independent byte/hash oracles, no visual claims."""

from __future__ import annotations

import base64
import json
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

from test_image_workflow import TNY, WASM, Handler, ImageFixture, png, sha


class PreviewHandler(Handler):
    def do_POST(self):
        if self.path not in ("/v1/chat/completions", "/v1/responses"):
            return super().do_POST()
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        state = self.server.state
        state["chat"].append(body)
        calls = state["calls"] if len(state["chat"]) == 1 else []
        if self.path.endswith("/responses"):
            events = []
            for i, (name, args) in enumerate(calls):
                item = {
                    "type": "function_call",
                    "id": f"item_{i}",
                    "call_id": f"call_{i}",
                    "name": name,
                    "arguments": json.dumps(args),
                    "status": "completed",
                }
                events.append(
                    {
                        "type": "response.output_item.done",
                        "output_index": i,
                        "item": item,
                    }
                )
            events.append(
                {"type": "response.completed", "response": {"status": "completed"}}
            )
            wire = "".join(
                f"event: {e['type']}\ndata: {json.dumps(e)}\n\n" for e in events
            )
        else:
            delta = {"content": "done"}
            if calls:
                delta = {
                    "tool_calls": [
                        {
                            "index": i,
                            "id": f"call_{i}",
                            "type": "function",
                            "function": {"name": name, "arguments": json.dumps(args)},
                        }
                        for i, (name, args) in enumerate(calls)
                    ]
                }
            events = [
                {"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {},
                            "finish_reason": "tool_calls" if calls else "stop",
                        }
                    ]
                },
            ]
            wire = (
                "".join(f"data: {json.dumps(e)}\n\n" for e in events)
                + "data: [DONE]\n\n"
            )
        self.reply(200, "text/event-stream", wire.encode())


class PreviewWorkflow(ImageFixture):
    def setUp(self):
        super().setUp()
        self.server.RequestHandlerClass = PreviewHandler
        self.settings = self.home / ".tny/settings.json"
        self.settings.parent.mkdir()
        self.configure(True)

    def configure(self, capability):
        self.settings.write_text(
            json.dumps(
                {} if capability is None else {"image_input": {"openai": capability}}
            )
        )

    def ask(self, calls, wire="chat", socket_cli=False, extra=()):
        self.state["calls"] = calls
        self.state["chat"].clear()
        env = dict(
            self.env,
            OPENAI_BASE_URL=self.url + "/v1",
            OPENAI_API_KEY="fixture",
            OPENAI_WIRE_API=wire,
            TNY_TOOLS="all",
            TNY_PROVIDER_RETRIES="0",
            TNY_ISOLATE="1" if socket_cli else "0",
        )
        return subprocess.run(
            [
                TNY,
                "--provider",
                "openai",
                "--cwd",
                str(self.home),
                *extra,
                "ask",
                *([] if socket_cli else ["--ephemeral"]),
                "fixture preview",
            ],
            env=env,
            capture_output=True,
            timeout=60,
        )

    def results(self):
        body = self.state["chat"][-1]
        if "messages" in body:
            return [m["content"] for m in body["messages"] if m.get("role") == "tool"]
        return [
            m["output"]
            for m in body["input"]
            if m.get("type") == "function_call_output"
        ]

    def pixels(self, body):
        result = []
        for message in body.get("messages", body.get("input", [])):
            content = message.get("content")
            if not isinstance(content, list):
                continue
            for part in content:
                if part.get("type") == "image_url":
                    url = part["image_url"]["url"]
                elif part.get("type") == "input_image":
                    url = part["image_url"]
                else:
                    continue
                result.append(base64.b64decode(url.split(",", 1)[1]))
        return result

    def result_object(self, index=0):
        text = self.results()[index]
        if text.startswith("error: "):
            text = text[7:]
        if not text.startswith("{"):
            text = text[text.index('{"kind"') :].splitlines()[0]
        return json.loads(text)

    def assert_preview(self, status="queued", count=1):
        self.assertEqual(len(self.state["chat"]), 2)
        self.assertEqual(self.pixels(self.state["chat"][0]), [])
        carried = self.pixels(self.state["chat"][1])
        self.assertEqual(carried, [self.state["image"]] * count)
        result = self.result_object()
        self.assertEqual(result["preview"]["status"], status)
        if count:
            self.assertEqual(result["preview"]["selected"]["sha256"], sha(carried[0]))
            self.assertEqual(result["preview"]["representation"], "original_bytes")
        return result

    def test_generate_default_false_true_both_wires(self):
        for wire in ("chat", "responses"):
            for preview in (None, False, True):
                with self.subTest(wire=wire, preview=preview):
                    args = {
                        "prompt": "blue",
                        "output_file": str(self.out),
                        "persist_manifest": False,
                    }
                    if preview is not None:
                        args["preview"] = preview
                    run = self.ask([("image_generate", args)], wire)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertEqual(len(self.state["chat"]), 2, run.stderr)
                    value = self.result_object()
                    self.assertTrue(value["ok"])
                    self.assertEqual(
                        self.pixels(self.state["chat"][-1]),
                        [self.state["image"]] if preview else [],
                    )
                    if preview:
                        self.assert_preview()
                        self.assertIsNone(value["preview"]["selected"]["manifest_path"])
                    else:
                        self.assertNotIn("preview", value)

    def test_intercept_generate_and_edit_both_wires(self):
        ref = self.home / "ref.png"
        ref.write_bytes(png(2, 2))
        for wire in ("chat", "responses"):
            for edit in (False, True):
                with self.subTest(wire=wire, edit=edit):
                    verb = "edit --image ref.png" if edit else "generate"
                    run = self.ask(
                        [
                            (
                                "terminal",
                                {
                                    "command": f"printf blue | tny image {verb} --output-file result.png --preview --json"
                                },
                            )
                        ],
                        wire,
                    )
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assert_preview()

    def test_typed_edit_and_replay(self):
        ref = self.home / "ref.png"
        ref.write_bytes(png(2, 2))
        run = self.ask(
            [
                (
                    "image_edit",
                    {
                        "prompt": "blue",
                        "images": [str(ref)],
                        "output_file": str(self.out),
                        "preview": True,
                    },
                )
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.assert_preview()["manifest_path"]
        run = self.ask(
            [
                (
                    "terminal",
                    {
                        "command": f"tny image replay --manifest {shlex.quote(record)} --output-file replay.png --preview --json"
                    },
                )
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assert_preview()

    def test_real_cli_through_runner_socket_both_wires(self):
        if WASM:
            self.skipTest(
                "no AF_UNIX runner on wasm; clean CLI refusal is tested separately"
            )
        script = self.home / "produce.sh"
        script.write_text(
            f"printf blue | {shlex.quote(TNY)} image generate --output-file result.png --preview --json\n"
        )
        for wire in ("chat", "responses"):
            with self.subTest(wire=wire):
                run = self.ask(
                    [("terminal", {"command": "sh produce.sh"})], wire, socket_cli=True
                )
                self.assertEqual(run.returncode, 0, run.stderr)
                value = self.assert_preview()
                self.assertTrue(value["preview"]["receipt_id"])

    def test_relative_cli_preview_tracks_producer_cwd(self):
        if WASM:
            self.skipTest(
                "native runner socket required; wasm refusal tested separately"
            )
        subdir = self.home / "subdir"
        subdir.mkdir()
        # A same-named parent artifact must never substitute for the child output.
        self.out.write_bytes(png(3, 3))
        script = self.home / "produce.sh"
        for wire in ("chat", "responses"):
            for cwd_flag in (False, True):
                for no_manifest in (False, True):
                    with self.subTest(
                        wire=wire, cwd_flag=cwd_flag, no_manifest=no_manifest
                    ):
                        prefix = "" if cwd_flag else "cd subdir\n"
                        flags = " --cwd subdir" if cwd_flag else ""
                        manifest = " --no-manifest" if no_manifest else ""
                        script.write_text(
                            prefix
                            + f"printf blue | {shlex.quote(TNY)}{flags} image generate "
                            f"--output-file result.png --preview --json{manifest}\n"
                        )
                        run = self.ask(
                            [("terminal", {"command": "sh produce.sh"})],
                            wire,
                            socket_cli=True,
                        )
                        self.assertEqual(run.returncode, 0, run.stderr)
                        value = self.assert_preview()
                        self.assertEqual(value["path"], "result.png")
                        self.assertEqual(
                            value["preview"]["selected"]["path"],
                            str(self.root / "subdir/result.png"),
                        )
                        self.assertEqual(
                            (subdir / "result.png").read_bytes(), self.state["image"]
                        )
                        self.assertEqual(self.out.read_bytes(), png(3, 3))
                        if no_manifest:
                            self.assertIsNone(
                                value["preview"]["selected"]["manifest_path"]
                            )

    def test_selected_failure_keeps_large_lineage_typed_and_intercept_json(self):
        refs = [self.home / (str(i) + "reference" * 22 + ".png") for i in range(5)]
        for ref in refs:
            ref.write_bytes(png(2, 2))
        args = [arg for ref in refs for arg in ("--image", str(ref))]
        record = self.result(
            self.cli(
                "edit", *args, "--output-file", str(self.out), "--json", prompt=b"blue"
            )
        )["manifest_path"]
        self.out.write_bytes(png(3, 3))
        calls = [
            ("image_preview", {"manifest": record}),
            (
                "terminal",
                {
                    "command": f"tny image preview --manifest {shlex.quote(record)} --json"
                },
            ),
        ]
        for wire in ("chat", "responses"):
            for call in calls:
                with self.subTest(wire=wire, tool=call[0]):
                    before = len(self.image_requests())
                    run = self.ask([call], wire)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    value = self.assert_preview("failed", 0)
                    self.assertFalse(value["ok"])
                    self.assertEqual(value["preview"]["error_code"], "hash_mismatch")
                    self.assertTrue(value["preview"]["fallback"])
                    sources = value["preview"]["selected"]["lineage"]["sources"]
                    self.assertEqual(
                        [s["path"] for s in sources], [str(p.resolve()) for p in refs]
                    )
                    self.assertEqual(
                        [s["sha256"] for s in sources], [sha(png(2, 2))] * 5
                    )
                    self.assertGreater(len(json.dumps(value)), 1024)
                    self.assertEqual(len(self.image_requests()), before)

    def test_linux_closed_before_control_write_preserves_artifact_and_json(self):
        if WASM or not sys.platform.startswith("linux"):
            self.skipTest("Linux-specific deterministic socket-close fault")
        source = self.home / "closed_control.c"
        library = self.home / "closed_control.so"
        # Test-only transport fault: an already-closed socketpair is installed
        # before connect returns. TCP/provider calls retain the real transport.
        source.write_text(r"""#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
int connect(int fd, const struct sockaddr *address, socklen_t len) {
    const char *target = getenv("TNY_SESSION_SOCK");
    if (address->sa_family == AF_UNIX && target &&
        strcmp(((const struct sockaddr_un *)address)->sun_path, target) == 0) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return -1;
        close(pair[1]);
        int result = dup2(pair[0], fd);
        close(pair[0]);
        if (result < 0) return -1;
        fputs("fixture: peer closed before connect returned\n", stderr);
        return 0;
    }
    int (*real_connect)(int, const struct sockaddr *, socklen_t);
    *(void **)(&real_connect) = dlsym(RTLD_NEXT, "connect");
    return real_connect(fd, address, len);
}
""")
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-shared",
                "-fPIC",
                str(source),
                "-ldl",
                "-o",
                str(library),
            ],
            check=True,
            capture_output=True,
        )
        self.env.update(
            LD_PRELOAD=str(library), TNY_SESSION_SOCK=str(self.home / "closed")
        )
        run = self.generate("--preview", "--no-manifest", "--json")
        self.assertIn(b"peer closed before connect returned", run.stderr)
        value = self.result(run)
        self.assertTrue(value["ok"])
        self.assertEqual(value["preview"]["status"], "failed")
        self.assertEqual(
            value["preview"]["selected"]["sha256"], sha(self.state["image"])
        )
        self.assertEqual(self.out.read_bytes(), self.state["image"])
        self.assertEqual(len(self.image_requests()), 1)
        for command in (
            ["image", "attach", "--json", str(self.out)],
            ["ask-user", "--json", "Question?"],
        ):
            with self.subTest(command=command):
                run = subprocess.run(
                    [TNY, *command], env=self.env, capture_output=True, timeout=10
                )
                self.assertIn(b"peer closed before connect returned", run.stderr)
                self.assertEqual(run.returncode, 2, run.stderr)
                self.assertEqual(run.stdout, b"")
                self.assertIn(b"could not write to session socket", run.stderr)

    def test_cli_edit_replay_export_sheet_reach_actual_next_request(self):
        if WASM:
            self.skipTest("wasm socket-only CLI controls are tested as unsupported")
        generated = self.result(self.generate("--json"))
        record = shlex.quote(generated["manifest_path"])
        commands = [
            "printf blue | TNY image edit --image result.png --output-file edit.png --preview --json",
            f"TNY image replay --manifest {record} --output-file replay.png --preview --json",
        ]
        if shutil.which("magick"):
            commands += [
                f"TNY image {verb} --image result.png --output-file {verb}.png --size 2x2 --no-manifest --preview --json"
                for verb in ("export", "contact-sheet")
            ]
        script = self.home / "produce.sh"
        for wire in ("chat", "responses"):
            for command in commands:
                with self.subTest(wire=wire, command=command):
                    script.write_text(command.replace("TNY", shlex.quote(TNY)) + "\n")
                    # Explicit exports do not overwrite existing outputs by default.
                    for name in ("export.png", "contact-sheet.png"):
                        (self.home / name).unlink(missing_ok=True)
                    run = self.ask(
                        [("terminal", {"command": "sh produce.sh"})],
                        wire,
                        socket_cli=True,
                    )
                    self.assertEqual(run.returncode, 0, run.stderr)
                    value = self.result_object()
                    self.assertEqual(value["preview"]["status"], "queued")
                    self.assertEqual(
                        self.pixels(self.state["chat"][-1]),
                        [(self.home / value["path"]).read_bytes()],
                    )
                    self.assertEqual(
                        value["preview"]["selected"]["sha256"],
                        sha((self.home / value["path"]).read_bytes()),
                    )
                    if "--no-manifest" in command:
                        self.assertTrue(value["preview"]["selected"]["derived"])
                        self.assertIsNone(value["preview"]["selected"]["manifest_path"])

    def test_manifest_selected_typed_intercept_cli_without_generation(self):
        record = self.result(self.generate("--json"))["manifest_path"]
        script = self.home / "select.sh"
        script.write_text(
            f"{shlex.quote(TNY)} image preview --manifest {shlex.quote(record)} --json\n"
        )
        calls = [
            ("image_preview", {"manifest": record}),
            (
                "terminal",
                {
                    "command": f"tny image preview --manifest {shlex.quote(record)} --json"
                },
            ),
        ]
        if not WASM:
            calls.append(("terminal", {"command": "sh select.sh"}))
        for wire in ("chat", "responses"):
            for call in calls:
                with self.subTest(wire=wire, call=call):
                    before = len(self.image_requests())
                    run = self.ask(
                        [call],
                        wire,
                        socket_cli=call[1].get("command") == "sh select.sh",
                    )
                    self.assertEqual(run.returncode, 0, run.stderr)
                    value = self.assert_preview()
                    self.assertEqual(
                        value["preview"]["selected"]["manifest_path"], record
                    )
                    self.assertEqual(len(self.image_requests()), before)

    def test_policy_unknown_false_and_step_budget(self):
        for capability in (None, False):
            with self.subTest(capability=capability):
                self.configure(capability)
                run = self.ask(
                    [
                        (
                            "image_generate",
                            {
                                "prompt": "blue",
                                "output_file": str(self.out),
                                "preview": True,
                            },
                        )
                    ]
                )
                self.assertEqual(run.returncode, 0, run.stderr)
                value = self.assert_preview("unsupported", 0)
                self.assertTrue(value["ok"])
                self.assertEqual(self.out.read_bytes(), self.state["image"])
        self.configure(True)
        run = self.ask(
            [
                (
                    "image_generate",
                    {"prompt": "blue", "output_file": str(self.out), "preview": True},
                )
            ],
            extra=("--max-steps", "1"),
        )
        self.assertEqual(len(self.state["chat"]), 1, run.stderr)
        self.assertEqual(self.pixels(self.state["chat"][0]), [])
        self.assertEqual(self.out.read_bytes(), self.state["image"])

    def test_selected_hash_replacement_refuses_not_substitutes(self):
        record = self.result(self.generate("--json"))["manifest_path"]
        self.out.write_bytes(png(3, 3))
        run = self.ask([("image_preview", {"manifest": record})])
        self.assertEqual(run.returncode, 0, run.stderr)
        value = self.assert_preview("failed", 0)
        self.assertFalse(value["ok"])
        self.assertEqual(value["preview"]["error_code"], "hash_mismatch")

    def test_selected_foreign_roots_are_rejected_by_real_admission(self):
        with tempfile.TemporaryDirectory(prefix="tny-preview-outside-") as outside:
            output = Path(outside) / "outside.png"
            value = self.result(self.generate("--json", output=output))
            record = self.home / "selected.json"
            record.write_bytes(Path(value["manifest_path"]).read_bytes())
            run = self.ask([("image_preview", {"manifest": str(record)})])
            self.assertEqual(run.returncode, 0, run.stderr)
            value = self.assert_preview("failed", 0)
            self.assertEqual(value["preview"]["error_code"], "outside_allowed_roots")
            self.assertEqual(output.read_bytes(), self.state["image"])

    def test_preview_boolean_misuse_never_reaches_producer(self):
        for preview in ("true", 1, None, {}):
            with self.subTest(preview=preview):
                run = self.ask(
                    [
                        (
                            "image_generate",
                            {
                                "prompt": "blue",
                                "output_file": str(self.out),
                                "preview": preview,
                            },
                        )
                    ]
                )
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(self.image_requests(), [])
                self.assertEqual(self.pixels(self.state["chat"][-1]), [])
                self.assertIn("error:", self.results()[0])

    def test_immutable_capture_after_same_batch_replacement_and_capacity(self):
        record = self.result(self.generate("--json"))["manifest_path"]
        run = self.ask(
            [
                ("image_preview", {"manifest": record}),
                ("write_file", {"path": str(self.out), "content": "replacement"}),
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assert_preview()
        self.assertEqual(self.out.read_text(), "replacement")
        self.out.write_bytes(self.state["image"])
        run = self.ask([("image_preview", {"manifest": record})] * 9)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.pixels(self.state["chat"][-1]), [self.state["image"]] * 8)
        self.assertEqual(self.result_object(8)["preview"]["error_code"], "queue_full")

    def test_oversize_artifact_retained_with_explicit_export_guidance(self):
        self.state["image"] = png(1, 1) + b"x" * (8 * 1024 * 1024)
        run = self.ask(
            [
                (
                    "image_generate",
                    {"prompt": "blue", "output_file": str(self.out), "preview": True},
                )
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        value = self.assert_preview("failed", 0)
        self.assertTrue(value["ok"])
        self.assertEqual(value["preview"]["error_code"], "image_too_large")
        self.assertIn("Explicitly export", value["preview"]["fallback"])
        self.assertEqual(self.out.read_bytes(), self.state["image"])
        self.assertEqual(len(self.image_requests()), 1)

    def test_retained_finalization_failure_and_generation_failure_queue_nothing(self):
        def obstruct(_body):
            record = self.manifests()[0]
            record.unlink()
            record.mkdir()

        self.state["on_image"] = obstruct
        run = self.ask(
            [
                (
                    "image_generate",
                    {"prompt": "blue", "output_file": str(self.out), "preview": True},
                )
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        value = self.assert_preview("not_attempted", 0)
        self.assertEqual(value["code"], "IMAGE_MANIFEST_FINALIZE_FAILED")
        self.assertTrue(value["committed"])
        self.assertEqual(self.out.read_bytes(), self.state["image"])
        self.state["on_image"] = None
        self.state["image"] = b"not an image"
        run = self.ask(
            [
                (
                    "image_generate",
                    {"prompt": "blue", "output_file": str(self.out), "preview": True},
                )
            ]
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assert_preview("not_attempted", 0)
        self.assertEqual(self.out.read_bytes(), png(1, 1))

    def test_no_session_preserves_success_and_selection_failure_exit(self):
        value = self.result(self.generate("--preview", "--json"))
        expected = "unsupported" if WASM else "unavailable_session"
        self.assertEqual(value["preview"]["status"], expected)
        run = self.cli("preview", "--manifest", value["manifest_path"], "--json")
        self.assertEqual(run.returncode, 1, run.stderr)
        self.assertEqual(json.loads(run.stdout)["preview"]["status"], expected)
        self.assertEqual(len(self.image_requests()), 1)

    def test_producer_replacement_after_commit_refuses_exact_expected_identity(self):
        if WASM:
            self.skipTest("fault relay uses the native runner socket")
        replacement = png(3, 3)
        relay = self.home / "replace_at_capture.py"
        # A fault relay does not decide admission. It replaces only this
        # fixture's output after the real CLI has committed and supplied its
        # digest, then forwards the same request to the actual owning runner.
        relay.write_text(f"""import json, os, socket, subprocess, threading
from pathlib import Path
real = os.environ['TNY_SESSION_SOCK']
proxy = {str(self.home / "proxy")!r}
server = socket.socket(socket.AF_UNIX)
server.bind(proxy)
server.listen()
def relay():
    client, _ = server.accept()
    with client, client.makefile('rb') as source:
        hello = source.readline()
        request = source.readline()
        identity = json.loads(request)
        Path(identity['path']).write_bytes(bytes.fromhex({replacement.hex()!r}))
        with socket.socket(socket.AF_UNIX) as owner:
            owner.connect(real)
            owner.sendall(hello + request)
            with owner.makefile('rb') as response:
                for line in response:
                    if json.loads(line).get('id') == identity['id']:
                        client.sendall(line)
                        break
    server.close()
thread = threading.Thread(target=relay)
thread.start()
env = dict(os.environ, TNY_SESSION_SOCK=proxy)
run = subprocess.run([{TNY!r}, 'image', 'generate', '--output-file', {str(self.out)!r},
                      '--preview', '--no-manifest', '--json'], input=b'blue', env=env)
thread.join(10)
raise SystemExit(run.returncode)
""")
        run = self.ask(
            [("terminal", {"command": "python3 replace_at_capture.py"})],
            socket_cli=True,
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        value = self.assert_preview("failed", 0)
        self.assertTrue(value["ok"])
        self.assertEqual(value["preview"]["error_code"], "hash_mismatch")
        self.assertEqual(
            value["preview"]["selected"]["sha256"], sha(self.state["image"])
        )
        self.assertEqual(self.out.read_bytes(), replacement)
        self.assertEqual(len(self.image_requests()), 1)

    def test_after_commit_transport_close_keeps_success_no_retry(self):
        if WASM:
            self.skipTest("wasm has no Unix socket; explicit clean refusal is tested")
        sock = socket.socket(socket.AF_UNIX)
        # macOS sockaddr_un bound: use a short pathname inside this private HOME.
        path = str(self.home / "s")
        sock.bind(path)
        sock.listen()
        received = []

        def receive():
            connection, _ = sock.accept()
            with connection, connection.makefile("rb") as stream:
                received.append(json.loads(stream.readline()))
                received.append(json.loads(stream.readline()))
                # Request accepted by transport, but deliberately no acknowledgment.
            sock.close()

        thread = threading.Thread(target=receive)
        thread.start()
        self.env["TNY_SESSION_SOCK"] = path
        run = self.generate("--preview", "--no-manifest", "--json")
        thread.join(5)
        self.assertFalse(thread.is_alive())
        value = self.result(run)
        self.assertTrue(value["ok"])
        self.assertEqual(value["preview"]["status"], "failed")
        self.assertEqual(received[1]["op"], "image_preview")
        self.assertEqual(received[1]["expected_sha256"], sha(self.state["image"]))
        self.assertEqual(self.out.read_bytes(), self.state["image"])
        self.assertEqual(len(self.image_requests()), 1)

    def test_cli_ack_is_correlated_and_split_boundaries_do_not_queue_twice(self):
        if WASM:
            self.skipTest("no AF_UNIX on wasm; clean refusal is separately tested")
        sock = socket.socket(socket.AF_UNIX)
        path = str(self.home / "s")
        sock.bind(path)
        sock.listen()
        requests = []

        def receive():
            connection, _ = sock.accept()
            with connection, connection.makefile("rb") as stream:
                requests.append(json.loads(stream.readline()))
                request = json.loads(stream.readline())
                requests.append(request)
                wire = (
                    json.dumps({"id": "unrelated", "ok": True, "status": "queued"})
                    + "\n"
                    + json.dumps(
                        {
                            "id": request["id"],
                            "ok": False,
                            "status": "turn_not_ready",
                            "error_code": "no_continuable_tool_batch",
                        }
                    )
                    + "\n"
                ).encode()
                for byte in wire:
                    connection.sendall(bytes([byte]))
            sock.close()

        thread = threading.Thread(target=receive)
        thread.start()
        self.env["TNY_SESSION_SOCK"] = path
        value = self.result(self.generate("--preview", "--json"))
        thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(value["preview"]["status"], "turn_not_ready")
        self.assertIsNone(value["preview"]["receipt_id"])
        self.assertEqual(
            len(requests), 2
        )  # hello and one preview, never retry/manual attach
        self.assertEqual(self.out.read_bytes(), self.state["image"])

    def test_export_sheet_and_selected_derived_manifest(self):
        if WASM:
            run = self.cli(
                "export",
                "--image",
                str(self.out),
                "--output-file",
                "derived.png",
                "--size",
                "1x1",
                "--preview",
                "--json",
            )
            self.assertNotEqual(run.returncode, 0)
            return
        if not shutil.which("magick"):
            self.skipTest("optional real ImageMagick 7 not on PATH; no fake converter")
        self.result(self.generate("--json"))
        for name in ("image_export", "image_contact_sheet"):
            target = self.home / f"{name}.png"
            run = self.ask(
                [
                    (
                        name,
                        {
                            "sources": [{"image": str(self.out)}],
                            "output_file": str(target),
                            "size": "2x2",
                            "preview": True,
                        },
                    )
                ]
            )
            self.assertEqual(run.returncode, 0, run.stderr)
            value = self.result_object()
            self.assertEqual(value["preview"]["status"], "queued")
            self.assertTrue(value["preview"]["selected"]["derived"])
            self.assertEqual(self.pixels(self.state["chat"][-1]), [target.read_bytes()])
            record = value["manifest_path"]
            before = len(self.image_requests())
            run = self.ask([("image_preview", {"manifest": record})], "responses")
            self.assertEqual(run.returncode, 0, run.stderr)
            selected = self.result_object()["preview"]["selected"]
            self.assertTrue(selected["derived"])
            self.assertEqual(
                selected["lineage"]["operation"],
                "export" if name == "image_export" else "contact_sheet",
            )
            self.assertEqual(
                selected["lineage"]["sources"][0]["sha256"], sha(self.out.read_bytes())
            )
            self.assertEqual(selected["lineage"]["transform_policy"], "fit")
            self.assertEqual(selected["sha256"], sha(target.read_bytes()))
            self.assertEqual(self.pixels(self.state["chat"][-1]), [target.read_bytes()])
            self.assertEqual(len(self.image_requests()), before)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        TNY = str(Path(sys.argv.pop(1)).resolve())
    unittest.main()
