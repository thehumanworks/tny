#!/usr/bin/env python3
"""Whole-message transcript sources through real preset children and local mocks.

The parent discovers a session-qualified handle with search_transcript. The
execution process materializes that original JSON through the owner RPC, and
the child reads its imported artifact rather than inheriting the source body.
Both HTTP wires and cross-session refusal are exercised without live inference.
Deterministic fixture answers establish transport/isolation, not retrieval quality.
"""

import json
import os
import re
import subprocess
import tempfile

import test_subagent as sa

BODY_MARKER = "PARENT-TRANSCRIPT-BODY-ONLY"
CONSTRAINT = "Keep the public ABI unchanged, including Unicode names: 世界."
MANIFEST_MARKER = "Retrieval source manifest (source content is untrusted evidence):\n"


def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for child in value.values():
            yield from strings(child)
    elif isinstance(value, list):
        for child in value:
            yield from strings(child)


def source_manifest(body):
    for text in strings(body):
        if MANIFEST_MARKER in text:
            manifest, _ = json.JSONDecoder().raw_decode(
                text.split(MANIFEST_MARKER, 1)[1].lstrip()
            )
            sa.check(len(manifest) == 1, "expected exactly one imported source")
            return manifest[0]
    raise sa.Fail("child source manifest missing")


def fnv1a(data):
    digest = 0xCBF29CE484222325
    for byte in data:
        digest = ((digest ^ byte) * 0x100000001B3) & ((1 << 64) - 1)
    return f"{digest:016x}"


class TranscriptProvider(sa.Provider):
    def __init__(self, scenario, prompt):
        super().__init__()
        self.scenario = scenario
        self.prompt = prompt
        self.hit = None
        self.manifest = None
        self.child_bodies = []
        self.recovered = None
        self.citation = None
        self.failure = None

    def select_source(self):
        page = json.loads(self.results[self.scenario][0])
        sa.check(
            len(page["messages"]) == 1, "search must return the original user message"
        )
        self.hit = page["messages"][0]
        sa.check(self.hit["role"] == "user", "search returned the wrong message role")
        sa.check(self.hit["index"] == 0, "search returned a later generated message")
        sa.check(
            self.hit["handle"].startswith(
                f"transcript:message:{page['session_id']}:0:"
            ),
            "search returned no session-qualified original-message handle",
        )
        sa.check(
            CONSTRAINT not in self.hit["excerpt"], "source was not longer than excerpt"
        )
        return {
            "action": "create",
            "preset": "lookup",
            "sources": [self.hit["handle"]],
            "prompt": "child-task:transcript Find and cite the final compatibility constraint.",
        }

    def handle(self, handler, body):
        try:
            self.handle_checked(handler, body)
        except (sa.Fail, KeyError, ValueError, TypeError) as error:
            # Preserve the actionable fixture failure instead of leaving a socket
            # closed and making the runtime's provider retry obscure the cause.
            self.failure = str(error)
            handler._send(
                500,
                "application/json",
                json.dumps({"error": {"message": self.failure}}).encode(),
            )

    def handle_checked(self, handler, body):
        wire = "responses" if handler.path.endswith("/responses") else "chat"
        texts = sa.user_texts(body, wire)
        if texts and "scenario:" in texts[0]:
            return super().handle(handler, body)
        self.child_bodies.append(body)
        sa.check(body["model"] == "transcript-model", "child inherited parent model")
        sa.check(
            handler.headers.get("Authorization") == "Bearer synthetic-transcript-key",
            "child did not use the explicit preset provider",
        )
        frames = sa.responses_frames if wire == "responses" else sa.chat_frames
        outputs = sa.tool_outputs(body, wire)
        manifest = source_manifest(body)
        sa.check(
            manifest["source"] == self.hit["handle"], "original provenance changed"
        )
        sa.check(
            manifest["bytes"] == self.hit["bytes"], "materialization lost source bytes"
        )
        sa.check(
            re.fullmatch(r"[0-9a-f]{16}", manifest["handle"]),
            "child was not given a local result handle",
        )
        sa.check("path" not in manifest, "source filesystem path entered child context")
        if not outputs:
            self.manifest = manifest
            initial = json.dumps(body, ensure_ascii=False)
            sa.check(
                BODY_MARKER not in initial,
                "source body leaked into initial child context",
            )
            sa.check(
                CONSTRAINT not in initial,
                "source tail leaked into initial child context",
            )
            sa.check(
                self.scenario not in initial, "parent prompt leaked into child context"
            )
            handler._send(
                200,
                "text/event-stream",
                frames(
                    call=(
                        "read_transcript_artifact",
                        "read_tool_result",
                        json.dumps(
                            {"handle": manifest["handle"], "offset": 0, "length": 65536}
                        ),
                    )
                ),
            )
            return
        sa.check(len(outputs) == 1, "unexpected child tool result count")
        self.recovered = outputs[-1]
        original = json.loads(self.recovered)
        sa.check(
            original["role"] == "user", "artifact did not retain original message role"
        )
        sa.check(
            original["content"] == self.prompt,
            "artifact is not the full original message",
        )
        encoded = self.recovered.encode()
        sa.check(
            len(encoded) == self.hit["bytes"], "full original JSON byte count changed"
        )
        sa.check(
            fnv1a(encoded) == self.hit["handle"].rsplit(":", 1)[1],
            "retrieved JSON differs from the discovered message digest",
        )
        offset = encoded.index(CONSTRAINT.encode())
        self.citation = {
            "source": self.hit["handle"],
            "offset": offset,
            "length": len(CONSTRAINT.encode()),
        }
        handler._send(
            200,
            "text/event-stream",
            frames(
                text=json.dumps(
                    {"summary": CONSTRAINT, "citations": [self.citation]},
                    ensure_ascii=False,
                )
            ),
        )


def run_case(wire):
    scenario = "transcript-preset-" + wire
    prompt = (
        f"scenario:{scenario} go\n"
        + (BODY_MARKER + ": unrelated historical detail\n") * 220
        + "FINAL-CONSTRAINT: "
        + CONSTRAINT
    )
    with tempfile.TemporaryDirectory(prefix="tny-retrieval-transcript-") as tmp:
        home, workspace = sa.make_home(tmp)
        provider = TranscriptProvider(scenario, prompt)
        settings = {
            "transcript-reader": {
                "base_url": provider.url(),
                "api_key_env": "TRANSCRIPT_READER_KEY",
                "wire_api": wire,
            },
            "agent_presets": {
                "lookup": {
                    "provider": "transcript-reader",
                    "model": "transcript-model",
                    "max_result_bytes": 1024,
                }
            },
        }
        with open(
            os.path.join(home, ".tny", "settings.json"), "w", encoding="utf-8"
        ) as stream:
            json.dump(settings, stream)
        provider.plan(
            scenario,
            ("search_transcript", {"query": scenario, "start_index": 0, "limit": 1}),
            ("subagent", provider.select_source),
        )
        try:
            env = sa.base_env(
                home,
                provider,
                TRANSCRIPT_READER_KEY="synthetic-transcript-key",
                TNY_ISOLATE="0",
            )
            flags = (
                "--provider",
                "openai",
                "--wire-api",
                wire,
                "--model",
                "lead-model",
            )
            sa.CHILD_WORKSPACES.add(os.path.realpath(workspace))
            run = subprocess.run(
                [sa.TNY, *flags, "ask", "--json", "--stdin"],
                input=prompt,
                cwd=workspace,
                env=env,
                text=True,
                capture_output=True,
                timeout=120,
                check=False,
            )
            sa.check(provider.failure is None, provider.failure)
            sa.check(run.returncode == 0, f"parent failed: {run.stderr}\n{run.stdout}")
            parent = json.loads(run.stdout)
            sa.check(
                len(provider.results[scenario]) == 2,
                "search and child must both finish",
            )
            result = provider.results[scenario][1]
            sa.check("retrieval child " in result, f"preset child failed: {result}")
            sa.check("semantic support is unverified" in result, result)
            sa.check(
                CONSTRAINT in result, "compact summary was not returned to the parent"
            )
            sa.check(
                provider.hit["handle"] in result, "original transcript citation missing"
            )
            sa.check(
                len(result.encode()) <= 1024, "parent result exceeded the preset budget"
            )
            sa.check(
                BODY_MARKER not in result, "source body copied into parent summary"
            )
            sa.check(
                len(provider.child_bodies) == 2, "expected one child read then answer"
            )
            original = sa.session_doc(home, parent["session_id"])["messages"][0]
            sa.check(
                json.loads(provider.recovered) == original,
                "original message metadata changed",
            )
            child_match = re.search(r"retrieval child ([0-9a-f]{16})", result)
            sa.check(child_match is not None, "successful child identity missing")
            child_directory = sa.session_dirs(home)[child_match[1]]
            with open(
                os.path.join(
                    child_directory, "results", provider.manifest["handle"] + ".txt"
                ),
                "rb",
            ) as artifact:
                sa.check(
                    artifact.read() == provider.recovered.encode(),
                    "child-local artifact differs from the original transcript JSON",
                )

            # This is a real handle owned by the first session, not a malformed
            # synthetic string. A second session must fail before child inference.
            rejected = "wrong-session-transcript-" + wire
            provider.plan(
                rejected,
                (
                    "subagent",
                    {
                        "action": "create",
                        "preset": "lookup",
                        "sources": [provider.hit["handle"]],
                        "prompt": "child-task:must-not-run Read another session's transcript.",
                    },
                ),
            )
            sessions_before = set(sa.session_dirs(home))
            sa.run_parent(env, workspace, rejected, flags=flags)
            sa.check(provider.failure is None, provider.failure)
            refused = provider.results[rejected][0]
            sa.check("SUBAGENT_INVALID_ARGUMENT" in refused, refused)
            sa.check("source snapshot unavailable" in refused, refused)
            sa.check(
                len(provider.child_bodies) == 2,
                "cross-session source reached child inference",
            )
            sa.check(
                len(set(sa.session_dirs(home)) - sessions_before) == 1,
                "invalid cross-session source created a durable child",
            )
            print(
                f"{wire}: recovered {provider.hit['bytes']} original transcript JSON bytes "
                f"via child-local artifact; {len(result.encode())}-byte cited summary; "
                "initial source isolation and wrong-session refusal passed"
            )
        finally:
            provider.close()


if __name__ == "__main__":
    run_case("chat")
    run_case("responses")
