#!/usr/bin/env python3
"""Settings editor contract and generated headless UI persistence checks.

Schema tests cover agent_presets, image_input, dictation.normalize and ui, including reserved
root names. Uses jsonschema when installed and always checks stdlib patterns.
The UI CLI tests use TNY (default build/tny) and a temporary HOME without
provider credentials, verifying strict values, field preservation and output
privacy. The C suite separately verifies runtime defaults and failed saves.

Accepts the binary path tests/integration/run.sh appends.
"""

from __future__ import annotations

import json
import os
import random
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SUPPLIED_BINARY = (
    sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None
)
SCHEMA_PATH = ROOT / "schemas/settings.schema.json"
SCHEMA = json.loads(SCHEMA_PATH.read_text())

try:  # optional: full-document validation when the library is available
    import jsonschema
except ImportError:  # pragma: no cover - depends on the environment
    jsonschema = None


DICTIONARY = json.loads((ROOT / "schemas/dictionary.schema.json").read_text())


class UiSettingsTests(unittest.TestCase):
    def test_ui_schema_defaults_and_reserved_name(self):
        ui = SCHEMA["properties"]["ui"]
        self.assertEqual(ui["properties"]["mode"]["enum"], ["inline", "fullscreen"])
        self.assertEqual(ui["properties"]["mode"]["default"], "inline")
        self.assertEqual(ui["properties"]["alternate_screen"]["type"], "boolean")
        self.assertTrue(ui["properties"]["alternate_screen"]["default"])
        self.assertEqual(
            ui["properties"]["scrollback_lines"],
            {
                "type": "integer",
                "minimum": 1,
                "maximum": 1000000,
                "default": 50000,
                "description": "Maximum logical transcript lines retained for fullscreen scrolling. Applies on the next TUI launch.",
            },
        )
        self.assertCountEqual(
            ui["properties"]["tool_call_view"]["enum"], ["compact", "full", "hidden"]
        )
        self.assertEqual(ui["properties"]["tool_call_view"]["default"], "compact")
        self.assertFalse(ui["additionalProperties"])
        self.assertFalse(
            any(re.fullmatch(p, "ui") for p in SCHEMA["patternProperties"])
        )
        if jsonschema is None:
            return
        for mode in ("inline", "fullscreen"):
            for alternate in (False, True):
                jsonschema.validate(
                    {"ui": {"mode": mode, "alternate_screen": alternate}}, SCHEMA
                )
        for view in ("compact", "full", "hidden"):
            jsonschema.validate({"ui": {"tool_call_view": view}}, SCHEMA)
        for ui in (
            [],
            "fullscreen",
            {"mode": "full"},
            {"alternate_screen": "false"},
            {"extra": 1},
            *({"tool_call_view": value} for value in ("Full", "full ", "", True)),
            *(
                ({"scrollback_lines": value})
                for value in (0, -1, 1000001, 1.5, "50000", True)
            ),
        ):
            with self.assertRaises(jsonschema.ValidationError):
                jsonschema.validate({"ui": ui}, SCHEMA)

    def test_generated_headless_ui_updates_preserve_other_fields(self):
        binary = Path(
            os.environ.get("TNY", SUPPLIED_BINARY or str(ROOT / "build/tny"))
        ).resolve()
        if not binary.is_file():
            self.skipTest("tny binary is unavailable")
        seed = random.Random(0x1A2B3C4D)
        with tempfile.TemporaryDirectory(prefix="tny-ui-settings-") as home:
            env = {
                "PATH": os.environ.get("PATH", ""),
                "HOME": home,
                "TNY_SELF_IMPROVE": "0",
            }
            settings = Path(home) / ".tny/settings.json"

            def run(*args):
                return subprocess.run(
                    [str(binary), *args],
                    cwd=home,
                    env=env,
                    text=True,
                    capture_output=True,
                    timeout=10,
                )

            result = run("settings", "--json")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                json.loads(result.stdout)["ui"],
                {
                    "mode": "inline",
                    "alternate_screen": True,
                    "scrollback_lines": 50000,
                    "tool_call_view": "compact",
                    "tool_calls_expanded": False,
                    "inline_mouse": False,
                    "tool_call_reasons": True,
                },
            )
            self.assertFalse(settings.exists())
            settings.parent.mkdir()
            for _ in range(32):
                mode = seed.choice(("inline", "fullscreen"))
                alternate = seed.choice((False, True))
                lines = seed.randint(1, 1000000)
                original = {
                    "provider": "missing-provider",
                    "agent_presets": {
                        "retrieve": {"provider": "codex", "model": "explicit-model"}
                    },
                    "models": {"openai": f"keep-{seed.getrandbits(32)}"},
                    "permission": {"edit": {"*": "deny"}},
                    "private_sentinel": "synthetic-secret-do-not-print",
                }
                settings.write_text(json.dumps(original))
                for key, value in (
                    ("ui.mode", mode),
                    ("ui.alternate_screen", str(alternate).lower()),
                    ("ui.tool_calls_expanded", str(alternate).lower()),
                    ("ui.inline_mouse", str(alternate).lower()),
                    ("ui.tool_call_reasons", str(alternate).lower()),
                    ("ui.scrollback_lines", str(lines)),
                ):
                    result = run("settings", "set", key, value, "--json")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn(
                        original["private_sentinel"], result.stdout + result.stderr
                    )
                saved = json.loads(settings.read_text())
                self.assertEqual(
                    saved.pop("ui"),
                    {
                        "mode": mode,
                        "alternate_screen": alternate,
                        "tool_call_view": "full" if alternate else "compact",
                        "inline_mouse": alternate,
                        "tool_call_reasons": alternate,
                        "scrollback_lines": lines,
                    },
                )
                self.assertEqual(saved, original)
                for key, expected in (
                    ("ui.mode", mode),
                    ("ui.alternate_screen", alternate),
                    ("ui.tool_call_view", "full" if alternate else "compact"),
                    ("ui.tool_calls_expanded", alternate),
                    ("ui.inline_mouse", alternate),
                    ("ui.tool_call_reasons", alternate),
                    ("ui.scrollback_lines", lines),
                ):
                    result = run("--json", "settings", "get", key)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(
                        json.loads(result.stdout),
                        {"kind": "setting", "key": key, "value": expected},
                    )
                before = settings.read_bytes()
                for key, bad in (
                    *(("ui.tool_call_view", value) for value in ("Full", "full ", "")),
                    ("ui.mode", "Fullscreen"),
                    ("ui.mode", "fullscreen "),
                    ("ui.alternate_screen", "1"),
                    ("ui.alternate_screen", "TRUE"),
                    *(
                        ("ui.scrollback_lines", value)
                        for value in (
                            "0",
                            "-1",
                            "+1",
                            "1.5",
                            "1000001",
                            " 2",
                            "2 ",
                            "99999999999999999999",
                        )
                    ),
                    ("private_sentinel", "synthetic-secret-do-not-print"),
                ):
                    result = run("settings", "set", key, bad)
                    self.assertEqual(result.returncode, 1)
                    self.assertNotIn(
                        original["private_sentinel"], result.stdout + result.stderr
                    )
                    self.assertEqual(settings.read_bytes(), before)

            for view in ("compact", "full", "hidden"):
                result = run("settings", "set", "ui.tool_call_view", view, "--json")
                self.assertEqual(result.returncode, 0, result.stderr)
                result = run("settings", "--json")
                resolved = json.loads(result.stdout)["ui"]
                self.assertEqual(resolved["tool_call_view"], view)
                self.assertEqual(resolved["tool_calls_expanded"], view == "full")

            for legacy in (False, True):
                for view in (
                    None,
                    "compact",
                    "full",
                    "hidden",
                    "invalid",
                    "full\x00hidden",
                ):
                    ui = {"tool_calls_expanded": legacy}
                    if view is not None:
                        ui["tool_call_view"] = view
                    settings.write_text(json.dumps({"ui": ui}))
                    before = settings.read_bytes()
                    result = run("settings", "--json")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    expected = (
                        view
                        if view in ("compact", "full", "hidden")
                        else ("full" if legacy else "compact")
                    )
                    resolved = json.loads(result.stdout)["ui"]
                    self.assertEqual(resolved["tool_call_view"], expected)
                    self.assertEqual(
                        resolved["tool_calls_expanded"], expected == "full"
                    )
                    self.assertEqual(settings.read_bytes(), before)
                    result = run(
                        "settings",
                        "set",
                        "ui.tool_calls_expanded",
                        str(not legacy).lower(),
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(
                        json.loads(settings.read_text())["ui"]["tool_call_view"],
                        "compact" if legacy else "full",
                    )


class DictationSchemaTests(unittest.TestCase):
    """dictation.normalize and dictionary.json (docs/adr/0175)."""

    def test_normalize_accepts_documented_shapes(self):
        if jsonschema is None:
            self.skipTest("jsonschema is not installed in this environment")
        for doc in (
            {"dictation": {"normalize": True}},
            {"dictation": {"normalize": {"enabled": True, "model": "gpt-6-luna"}}},
            {
                "dictation": {
                    "normalize": {
                        "enabled": True,
                        "model": {"codex": "gpt-6-luna", "xai": "grok-4.7"},
                        "effort": "omit",
                        "fast": False,
                        "timeout_seconds": 120,
                    }
                }
            },
        ):
            jsonschema.validate(doc, SCHEMA)
        for doc in (
            {"dictation": {"normalize": "yes"}},
            {"dictation": {"normalize": {"timeout_seconds": 0}}},
            {"dictation": {"normalize": {"model": {"claude": "x"}}}},
            {"dictation": {"normalize": {"conversation_model": True}}},
            {"dictation": {"stt": "codex"}},
        ):
            with self.assertRaises(jsonschema.ValidationError, msg=doc):
                jsonschema.validate(doc, SCHEMA)

    def test_dictionary_schema_matches_runtime_limits(self):
        (pattern,) = DICTIONARY["patternProperties"].keys()
        word = re.compile(pattern)
        for key in ("tny", "kubectl", "Claude Code", ".NET", "C++", "世界"):
            self.assertTrue(word.fullmatch(key), key)
        for key in ("", " padded", "...", "x" * 65, "bad\u001bword"):
            self.assertFalse(word.fullmatch(key), key)
        entry = DICTIONARY["patternProperties"][pattern]["oneOf"][1]
        self.assertEqual(entry["properties"]["aliases"]["maxItems"], 8)
        self.assertEqual(entry["properties"]["case"]["enum"], ["exact", "insensitive"])
        self.assertFalse(entry["additionalProperties"])
        if jsonschema is None:
            return
        jsonschema.validate(
            {
                "$schema": "https://example.invalid/dictionary.schema.json",
                "tny": "the agent harness",
                "kubectl": {"aliases": ["kube cuddle"]},
                "Jev": {"context": "decision engine", "case": "exact"},
            },
            DICTIONARY,
        )
        for doc in ({"tny": 1}, {"tny": {"weight": 2}}, {"tny": {"case": "upper"}}):
            with self.assertRaises(jsonschema.ValidationError, msg=doc):
                jsonschema.validate(doc, DICTIONARY)


class SpeedTierSchemaTests(unittest.TestCase):
    def test_ultrafast_is_a_tier_not_a_boolean_alias(self):
        tiers = SCHEMA["$defs"]["fastValue"]["oneOf"][1]["enum"]
        self.assertIn("ultrafast", tiers)
        self.assertIn("fast", tiers)
        self.assertIn("priority", tiers)
        if jsonschema is not None:
            for value in ("ultrafast", {"openai": "ultrafast"}):
                jsonschema.validate({"fast": value}, SCHEMA)
            with self.assertRaises(jsonschema.ValidationError):
                jsonschema.validate({"fast": "ULTRAFAST"}, SCHEMA)


class AgentPresetSchemaTests(unittest.TestCase):
    def test_context_load_validates_global_presets_without_resolving_them(self):
        binary = Path(
            os.environ.get("TNY", SUPPLIED_BINARY or str(ROOT / "build/tny"))
        ).resolve()
        if not binary.is_file():
            self.skipTest("tny binary is unavailable")
        with tempfile.TemporaryDirectory(prefix="tny-agent-preset-settings-") as home:
            env = {"PATH": os.environ.get("PATH", ""), "HOME": home}
            settings = Path(home) / ".tny/settings.json"
            settings.parent.mkdir()

            def status():
                return subprocess.run(
                    [str(binary), "--provider", "openai", "status", "--json"],
                    cwd=home,
                    env=env,
                    text=True,
                    capture_output=True,
                    timeout=10,
                )

            # api_key is a valid preset name, not a persisted provider credential.
            # Unknown providers and task names are not resolved during config load.
            settings.write_text(
                json.dumps(
                    {
                        "agent_presets": {
                            "api_key": {
                                "provider": "not-a-configured-provider",
                                "model": "not-the-conversation-model",
                                "task": "not-an-installed-task",
                            }
                        }
                    }
                )
            )
            result = status()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["backend"], "openai")
            self.assertNotIn("not-the-conversation-model", result.stdout)

            for value in (
                None,
                [],
                {"retrieve": {"provider": "p"}},
                {"retrieve": {"model": "m"}},
                {"retrieve": {"provider": "p", "model": "m", "api_key": "secret"}},
            ):
                settings.write_text(json.dumps({"agent_presets": value}))
                result = status()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("agent_presets", result.stderr)
                self.assertNotIn("secret", result.stdout + result.stderr)

            # Repository content never gains inference or credential authority.
            settings.write_text("{}")
            (Path(home) / ".tny.json").write_text(
                json.dumps({"agent_presets": {"retrieve": {"api_key": "secret"}}})
            )
            result = status()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("secret", result.stdout + result.stderr)

    def test_presets_are_global_strict_bounded_and_reserved(self):
        presets = SCHEMA["properties"]["agent_presets"]
        self.assertEqual(presets["maxProperties"], 32)
        entry = SCHEMA["$defs"]["agentPreset"]
        self.assertEqual(entry["required"], ["provider", "model"])
        self.assertFalse(entry["additionalProperties"])
        self.assertEqual(entry["properties"]["task"]["default"], "context-retrieval")
        self.assertEqual(entry["properties"]["max_result_bytes"]["default"], 8192)
        self.assertNotIn("default", entry["properties"]["provider"])
        self.assertNotIn("default", entry["properties"]["model"])
        self.assertNotIn("agent_presets", SCHEMA["$defs"]["workspace"]["properties"])
        for pattern in SCHEMA["patternProperties"]:
            self.assertIsNone(re.fullmatch(pattern, "agent_presets"))
        names = SCHEMA["$defs"]["agentPresetName"]
        self.assertEqual(names["maxLength"], 63)
        pattern = re.compile(names["pattern"])
        for name in ("retrieve", "a-b_c.d", "A9", "a" * 63):
            self.assertIsNotNone(pattern.search(name), name)
        for name in ("", ".hidden", "a..b", "a/b", "a b", "x\n", "a\x00b"):
            self.assertIsNone(pattern.search(name), name)

    def test_preset_documents_accept_explicit_independent_selection(self):
        if jsonschema is None:
            self.skipTest("jsonschema is not installed in this environment")
        for doc in (
            {"agent_presets": {}},
            {"agent_presets": {"retrieve": {"provider": "codex", "model": "small"}}},
            {
                "agent_presets": {
                    "retrieve": {
                        "provider": "gateway",
                        "model": "fast",
                        "effort": "light",
                        "task": "custom.instructions",
                        "description": "Find 世界 files",
                        "max_result_bytes": 65536,
                    }
                }
            },
            {
                "agent_presets": {
                    f"retrieve-{i}": {"provider": "p", "model": "m"} for i in range(32)
                }
            },
        ):
            jsonschema.validate(doc, SCHEMA)

    def test_preset_documents_reject_ambiguity_and_authority(self):
        if jsonschema is None:
            self.skipTest("jsonschema is not installed in this environment")
        invalid = [
            {"agent_presets": None},
            {"agent_presets": []},
            {"agent_presets": {".hidden": {"provider": "p", "model": "m"}}},
            {"agent_presets": {"a..b": {"provider": "p", "model": "m"}}},
            {"agent_presets": {"a" * 64: {"provider": "p", "model": "m"}}},
            {
                "agent_presets": {
                    f"retrieve-{i}": {"provider": "p", "model": "m"} for i in range(33)
                }
            },
        ]
        for entry in ({}, {"provider": "p"}, {"model": "m"}):
            invalid.append({"agent_presets": {"retrieve": entry}})
        for field in ("provider", "model", "effort"):
            for value in (None, "", True, "a\x00b", "a" * 257):
                entry = {"provider": "p", "model": "m", field: value}
                invalid.append({"agent_presets": {"retrieve": entry}})
        for field, value in (
            ("task", "../escape"),
            ("task", "x\n"),
            ("description", "a\x00b"),
            ("description", "a" * 257),
            *(
                ("max_result_bytes", value)
                for value in (1023, 65537, -1, 1.5, True, "8192")
            ),
            *(
                (field, "forbidden")
                for field in (
                    "api_key",
                    "api_key_env",
                    "base_url",
                    "headers",
                    "command",
                    "args",
                    "tools",
                    "permissions",
                    "permission_mode",
                    "subscription",
                    "name",
                )
            ),
        ):
            entry = {"provider": "p", "model": "m", field: value}
            invalid.append({"agent_presets": {"retrieve": entry}})
        for doc in invalid:
            with self.assertRaises(jsonschema.ValidationError, msg=doc):
                jsonschema.validate(doc, SCHEMA)


class SettingsSchemaTests(unittest.TestCase):
    def setUp(self):
        self.image_input = SCHEMA["properties"]["image_input"]

    def test_image_input_is_a_boolean_map_of_provider_selectors(self):
        self.assertEqual(self.image_input["type"], "object")
        self.assertEqual(self.image_input["additionalProperties"], {"type": "boolean"})
        self.assertEqual(self.image_input["maxProperties"], 1024)
        names = self.image_input["propertyNames"]
        self.assertEqual(names["maxLength"], 256)
        pattern = re.compile(names["pattern"])
        for key in ("codex", "claude", "my-gateway", "acp@agent"):
            self.assertTrue(pattern.fullmatch(key), key)
        for key in (
            "",
            "open ai",
            "acp:agent",
            "acp@",
            "acp@bad name",
            "gate/way",
        ):
            self.assertFalse(pattern.fullmatch(key), key)

    def test_image_input_describes_configured_not_verified_support(self):
        description = self.image_input["description"]
        self.assertIn("configured, unverified", description)
        self.assertIn("unknown", description)
        self.assertNotIn("verified support", description)

    def test_image_input_is_reserved_against_named_provider_profiles(self):
        (profile_pattern,) = SCHEMA["patternProperties"].keys()
        pattern = re.compile(profile_pattern)
        self.assertFalse(pattern.fullmatch("image_input"))
        for reserved in ("fast", "effort", "models", "acp", "mcp", "dictation"):
            self.assertFalse(pattern.fullmatch(reserved), reserved)
        self.assertTrue(pattern.fullmatch("openrouter"))

    def test_documents_validate_when_jsonschema_is_installed(self):
        if jsonschema is None:
            self.skipTest("jsonschema is not installed in this environment")
        valid = [
            {"image_input": {}},
            {"image_input": {"codex": True, "claude": False}},
            {"image_input": {"my-gateway": True}, "provider": "my-gateway"},
            {"image_input": {"acp@agent": True}},
        ]
        invalid = [
            {"image_input": []},
            {"image_input": "codex"},
            {"image_input": {"codex": "yes"}},
            {"image_input": {"codex": 1}},
            {"image_input": {"acp:agent": True}},
            {"image_input": {"open ai": True}},
            {"image_input": {"a" * 257: True}},
        ]
        for doc in valid:
            jsonschema.validate(doc, SCHEMA)
        for doc in invalid:
            with self.assertRaises(jsonschema.ValidationError, msg=doc):
                jsonschema.validate(doc, SCHEMA)


if __name__ == "__main__":
    while len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        sys.argv.pop(1)
    unittest.main(verbosity=2)
