#!/usr/bin/env python3
"""Long durable history must not become execution-process transport state."""

import json
import os
import subprocess
import sys
import unittest

import test_execution_code_mode as fixture


@unittest.skipIf(fixture.WASM, "fresh execution processes are native-only")
class ExecutionSnapshot(unittest.TestCase):
    def test_compacted_large_history_survives_tools_and_resume(self):
        for wire in ("chat", "responses"):
            with self.subTest(wire=wire):
                owner = fixture.ExecutionCodeMode()
                owner.setUp()
                try:
                    command = [
                        fixture.TNY,
                        "--cwd",
                        str(owner.workspace),
                        "--provider",
                        "openai",
                        "--wire-api",
                        wire,
                        "ask",
                        "--json",
                    ]
                    owner.server.call_name = None
                    owner.server.arguments = {}
                    seed = subprocess.run(
                        [*command, "seed"],
                        env=owner.env,
                        capture_output=True,
                        timeout=20,
                        check=True,
                    )
                    sid = json.loads(seed.stdout)["session_id"]
                    path = next(
                        (owner.home / ".tny" / "sessions").rglob("session.json")
                    )
                    stored = json.loads(path.read_text())
                    history = [
                        {"role": "user", "content": f"turn {i}: " + "x" * 16384}
                        for i in range(640)
                    ]
                    facts = [
                        "port is 48231, not 48213",
                        "never migrate the production database",
                        "rename only internal symbols, retain public ABI",
                        "earlier green check was stale; current check failed",
                        "cancel deployment until owner approval",
                        "keep compatibility with Python 3.11",
                        "payment credential must never enter logs",
                        "do not overwrite a concurrent user edit",
                        "timeout is 750 milliseconds, not seconds",
                        "the rollback commit is abc0123, not abc0213",
                        "test fixture is synthetic, not live evidence",
                        "latest decision supersedes the earlier plan",
                    ]
                    for index, fact in enumerate(facts):
                        history[index * 53]["content"] += (
                            f" RETENTION-FACT-{index}: {fact}"
                        )
                    diagnostic = "HISTORICAL-DIAGNOSTIC-END€"
                    digest = 0xCBF29CE484222325
                    for byte in diagnostic.encode():
                        digest = ((digest ^ byte) * 0x100000001B3) & ((1 << 64) - 1)
                    reference = f"transcript:{len(history)}:{digest:016x}"
                    history.append(
                        {"role": "tool", "tool_call_id": "old", "content": diagnostic}
                    )
                    stored["messages"] = history
                    stored["compact"] = {
                        "before": len(history),
                        "summary": "Earlier task.",
                    }
                    stored["extension_audit"] = [{"marker": "keep audit"}]
                    path.write_text(json.dumps(stored))
                    self.assertGreater(path.stat().st_size, 8 * 1024 * 1024)
                    owner.server.call_name = "run_code"
                    read_args = json.dumps({"handle": reference, "length": 64})
                    calls = json.dumps(
                        [
                            {"name": "read_file", "arguments": {"path": "effects"}},
                            {
                                "name": "read_tool_result",
                                "arguments": {"handle": reference, "length": 64},
                            },
                            {"name": "read_file", "arguments": {"path": "effects"}},
                            {
                                "name": "read_tool_result",
                                "arguments": {
                                    "handle": reference,
                                    "offset": 11,
                                    "length": 10,
                                },
                            },
                        ]
                    )
                    stale_args = json.dumps(
                        {"handle": "transcript:640:0000000000000000"}
                    )
                    eof_args = json.dumps(
                        {"handle": reference, "offset": 999, "length": 64}
                    )
                    split_utf8 = {
                        "handle": reference,
                        "offset": len(diagnostic.encode()) - 3,
                        "length": 1,
                    }
                    binary_args = json.dumps({**split_utf8, "encoding": "base64"})
                    text_args = json.dumps(split_utf8)
                    owner.server.arguments = {
                        "code": "import json\nfrom pathlib import Path\n"
                        "p=Path('effects')\n"
                        "p.write_text(p.read_text()+'x' if p.exists() else 'x')\n"
                        "print(tools.call('read_file', '{\"path\":\"effects\"}'))\n"
                        f"assert tools.call('read_tool_result', {read_args!r}) == {diagnostic!r}\n"
                        f"r=json.loads(tools.parallel({calls!r}))\n"
                        f"assert r[1] == {diagnostic!r} and r[3] == 'DIAGNOSTIC'\n"
                        f"assert tools.call('read_tool_result', {stale_args!r}).startswith('error: unknown handle')\n"
                        f"assert tools.call('read_tool_result', {eof_args!r}) == ''\n"
                        f"assert tools.call('read_tool_result', {binary_args!r}) == '4g=='\n"
                        f"assert 'incomplete UTF-8' in tools.call('read_tool_result', {text_args!r})\n"
                        "page=json.loads(tools.call('search_transcript', "
                        "json.dumps({'query':'HISTORICAL-DIAGNOSTIC','start_index':640,'limit':1})))\n"
                        "assert len(page['messages']) == 1\n"
                        "hit=page['messages'][0]\n"
                        f"assert hit['handle'].startswith('transcript:message:{sid}:640:')\n"
                        "original=tools.call('read_tool_result', json.dumps({'handle':hit['handle']}))\n"
                        f"assert json.loads(original) == {history[-1]!r}\n"
                        "assert hit['bytes'] == len(original.encode())\n"
                        f"facts={facts!r}\n"
                        "metadata_bytes=0\n"
                        "for number, fact in enumerate(facts):\n"
                        "    cursor=0\n"
                        "    while True:\n"
                        "        page_text=tools.call('search_transcript', json.dumps({'query':f'RETENTION-FACT-{number}:','start_index':cursor,'limit':1}))\n"
                        "        metadata_bytes+=len(page_text.encode())\n"
                        "        page=json.loads(page_text)\n"
                        "        if page['messages']:\n"
                        "            source=page['messages'][0]\n"
                        "            assert fact not in source['excerpt']\n"
                        "            recovered=tools.call('read_tool_result', json.dumps({'handle':source['handle'],'offset':16384,'length':1024}))\n"
                        "            assert fact in recovered\n"
                        "            break\n"
                        "        assert not page['complete']\n"
                        "        cursor=page['next_index']\n"
                        "print('RETRIEVAL-METRICS', json.dumps({'exact_facts':len(facts),'metadata_bytes':metadata_bytes}))\n"
                        "print('RECOVERED-HISTORY')"
                    }
                    for turn in range(2):
                        current = json.loads(path.read_text())
                        current["compact"]["before"] = len(current["messages"])
                        path.write_text(json.dumps(current))
                        owner.server.outputs.clear()
                        run = subprocess.run(
                            [*command, "--resume", sid, "continue"],
                            env=owner.env,
                            capture_output=True,
                            timeout=30,
                        )
                        self.assertEqual(run.returncode, 0, (run.stdout, run.stderr))
                        self.assertEqual(len(owner.server.outputs), 1)
                        self.assertNotIn("error:", owner.server.outputs[0])
                        self.assertIn("RECOVERED-HISTORY", owner.server.outputs[0])
                        if os.environ.get("TNY_TRANSCRIPT_BENCH"):
                            print(wire, turn, owner.server.outputs[0], flush=True)
                        self.assertEqual(
                            (owner.workspace / "effects").read_text(), "x" * (turn + 1)
                        )
                        saved = json.loads(path.read_text())
                        self.assertEqual(saved["messages"][: len(history)], history)
                        self.assertEqual(
                            saved["extension_audit"], stored["extension_audit"]
                        )
                finally:
                    owner.doCleanups()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
