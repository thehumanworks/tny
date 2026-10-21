#!/usr/bin/env python3
"""Preset routing, whole-source isolation and pinned resume with local mocks only.

The fixture chooses the answer deterministically. It proves plumbing and byte
bounds, not model retrieval recall, semantic quality, real tokens or API cost.
"""

import json
import os
import re
import subprocess
import tempfile
import time

import test_subagent as sa

SOURCE = "0123456789abcdef"
BODY = b"unrelated section\n" * 12000 + b"RELEVANT-TAIL: preserve this constraint\n"
OFFSET = BODY.index(b"RELEVANT-TAIL")


class RetrievalProvider(sa.Provider):
    def __init__(self, home, mode="normal"):
        super().__init__()
        self.home = home
        self.mode = mode
        self.child_bodies = []
        self.child_results = []
        self.seeded = False
        self.read_turns = set()

    def handle(self, handler, body):
        wire = "responses" if handler.path.endswith("/responses") else "chat"
        texts = sa.user_texts(body, wire)
        if texts and "scenario:" in texts[0]:
            for directory in [] if self.seeded else sa.session_dirs(self.home).values():
                path = os.path.join(directory, "results", SOURCE + ".txt")
                if not os.path.exists(path):
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    with open(path, "wb") as source:
                        source.write(BODY)
            self.seeded = True
            return super().handle(handler, body)
        self.child_bodies.append(body)
        frames = sa.responses_frames if wire == "responses" else sa.chat_frames
        serialized = json.dumps(body)
        sa.check(
            "unrelated section" not in serialized, "source leaked into child prompt"
        )
        match = re.search(r'\\?"handle\\?":\\?"([0-9a-f]{16})', serialized)
        sa.check(match is not None, "child source manifest missing")
        outputs = sa.tool_outputs(body, wire)
        turn = (len(texts), texts[-1] if texts else "")
        if turn not in self.read_turns:
            self.read_turns.add(turn)
            handler._send(
                200,
                "text/event-stream",
                frames(
                    call=(
                        "read_tail",
                        "read_tool_result",
                        json.dumps(
                            {"handle": match[1], "offset": OFFSET, "length": 128}
                        ),
                    )
                ),
            )
            return
        self.child_results.append(outputs[-1])
        sa.check(
            "RELEVANT-TAIL" in outputs[-1], "specialist could not read tail beyond64KiB"
        )
        # Child receives session-qualified provenance, not ambient session access.
        source = re.search(r"result:[0-9a-f]{16}:" + SOURCE, serialized)
        sa.check(source is not None, "original session-qualified source missing")
        answer = {
            "summary": "Preserve this constraint.",
            "citations": [
                {"source": source[0], "offset": OFFSET, "length": len(BODY) - OFFSET}
            ],
        }
        if self.mode == "invalid":
            answer["citations"][0]["source"] = "result:foreign:0000000000000000"
        elif self.mode == "oversize":
            answer["summary"] = "DERIVED-OVERSIZE " * 1000
        encoded = json.dumps(answer)
        if self.mode == "duplicate_root":
            encoded = encoded[:-1] + ', "citations": [{"source":"unchecked"}]}'
        elif self.mode == "duplicate_source":
            original = json.dumps(answer["citations"][0]["source"])
            encoded = encoded.replace(
                '"source": ' + original,
                '"source": ' + original + ', "source":"unchecked"',
                1,
            )
        elif self.mode == "duplicate_range":
            encoded = encoded.replace(
                f'"offset": {OFFSET}', f'"offset": {OFFSET}, "offset":999999999', 1
            )
        handler._send(200, "text/event-stream", frames(text=encoded))


def run_case(wire, mode="normal"):
    with tempfile.TemporaryDirectory(prefix="tny-retrieval-") as tmp:
        home, workspace = sa.make_home(tmp)
        provider = RetrievalProvider(home, mode)
        settings = {
            "retriever": {
                "base_url": provider.url(),
                "api_key_env": "RETRIEVER_KEY",
                "wire_api": wire,
            },
            "agent_presets": {
                "lookup": {
                    "provider": "retriever",
                    "model": "retrieval-model",
                    "effort": "low",
                }
            },
        }
        settings_path = os.path.join(home, ".tny", "settings.json")
        with open(settings_path, "w", encoding="utf-8") as stream:
            json.dump(settings, stream)
        scenario = "retrieval-" + wire + "-" + mode

        def message_args():
            text = provider.results[scenario][0]
            sid = re.search(r"retrieval child ([0-9a-f]{16})", text)
            sa.check(sid is not None, f"missing successful preset child: {text}")
            original = re.search(r"result:([0-9a-f]{16}):" + SOURCE, text)
            sa.check(original is not None, "missing original parent source identity")
            os.unlink(
                os.path.join(
                    sa.session_dirs(home)[original[1]], "results", SOURCE + ".txt"
                )
            )
            # Repointing the named preset must not silently change existing child.
            settings["agent_presets"]["lookup"]["model"] = "wrong-changed-model"
            with open(settings_path, "w", encoding="utf-8") as stream:
                json.dump(settings, stream)
            return {"action": "message", "id": sid[1], "prompt": "child-task:again"}

        provider.plan(
            scenario,
            (
                "subagent",
                {
                    "action": "create",
                    "preset": "lookup",
                    "sources": [SOURCE],
                    "prompt": "child-task:retrieve Find the relevant tail constraint.",
                },
            ),
            *(([("subagent", message_args)]) if mode == "normal" else []),
        )
        try:
            env = sa.base_env(
                home, provider, RETRIEVER_KEY="synthetic-retriever-key", TNY_ISOLATE="0"
            )
            started = time.monotonic()
            sa.run_parent(
                env,
                workspace,
                scenario,
                flags=(
                    "--provider",
                    "openai",
                    "--wire-api",
                    wire,
                    "--model",
                    "lead-model",
                ),
            )
            elapsed = time.monotonic() - started
            if mode != "normal":
                result = provider.results[scenario][0]
                sa.check(len(result.encode()) < 1024, "control response exceeded bound")
                if mode == "invalid" or mode.startswith("duplicate_"):
                    sa.check("Result withheld" in result, result)
                    sa.check(
                        "Preserve this constraint" not in result,
                        "invalid answer leaked",
                    )
                else:
                    match = re.search(r"handle ([0-9a-f]{16})", result)
                    sa.check(match is not None, result)
                    candidates = [
                        os.path.join(directory, "results", match[1] + ".txt")
                        for directory in sa.session_dirs(home).values()
                    ]
                    retained = [path for path in candidates if os.path.isfile(path)]
                    sa.check(len(retained) == 1, "missing intact oversized envelope")
                    with open(retained[0], encoding="utf-8") as stream:
                        envelope = json.load(stream)
                    sa.check(len(envelope["summary"]) == 17000, "summary was clipped")
                    sa.check(
                        envelope["citations"][0]["offset"] == OFFSET,
                        "citation metadata was clipped",
                    )
                print(
                    f"{wire}: {mode} envelope safely bounded ({len(result.encode())} bytes)"
                )
                return
            sa.check(
                len(provider.results[scenario]) == 2, "both child turns must return"
            )
            for result in provider.results[scenario]:
                sa.check("Preserve this constraint." in result, result)
                sa.check("semantic support is unverified" in result, result)
                sa.check(
                    len(result.encode()) < 1024, "lead received an oversized result"
                )
            sa.check(
                all(b["model"] == "retrieval-model" for b in provider.child_bodies),
                "changed preset or parent model silently rerouted the child",
            )
            child = re.search(
                r"retrieval child ([0-9a-f]{16})", provider.results[scenario][0]
            )[1]
            before = sa.session_dirs(home)
            recovered = subprocess.run(
                [sa.TNY, "session", "recover", child],
                cwd=workspace,
                env=env,
                text=True,
                capture_output=True,
                timeout=30,
                check=False,
            )
            sa.check(
                recovered.returncode != 0
                and "retrieval sessions cannot be copied" in recovered.stderr,
                "generic recovery returned an unusable retrieval copy",
            )
            sa.check(
                sa.session_dirs(home) == before,
                "refused recovery created partial session files",
            )
            print(
                f"{wire}: {len(BODY)} source bytes -> "
                f"{max(len(r.encode()) for r in provider.results[scenario])} lead result bytes; "
                f"tail retrieved; preset pinned across settings change; {elapsed:.3f}s local mock wall time"
            )
        finally:
            provider.close()


if __name__ == "__main__":
    run_case("chat")
    run_case("responses")
    for wire in ("chat", "responses"):
        run_case(wire, "invalid")
        run_case(wire, "oversize")
        for mode in ("duplicate_root", "duplicate_source", "duplicate_range"):
            run_case(wire, mode)
