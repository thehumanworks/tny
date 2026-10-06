#!/usr/bin/env python3
"""Settings editor contract and generated headless UI persistence checks.

Schema tests cover image_input, dictation.normalize and ui, including reserved
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
        for ui in (
            [],
            "fullscreen",
            {"mode": "full"},
            {"alternate_screen": "false"},
            {"extra": 1},
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
                {"mode": "inline", "alternate_screen": True, "scrollback_lines": 50000},
            )
            self.assertFalse(settings.exists())
            settings.parent.mkdir()
            for _ in range(32):
                mode = seed.choice(("inline", "fullscreen"))
                alternate = seed.choice((False, True))
                lines = seed.randint(1, 1000000)
                original = {
                    "provider": "missing-provider",
                    "models": {"openai": f"keep-{seed.getrandbits(32)}"},
                    "permission": {"edit": {"*": "deny"}},
                    "private_sentinel": "synthetic-secret-do-not-print",
                }
                settings.write_text(json.dumps(original))
                for key, value in (
                    ("ui.mode", mode),
                    ("ui.alternate_screen", str(alternate).lower()),
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
                        "scrollback_lines": lines,
                    },
                )
                self.assertEqual(saved, original)
                for key, expected in (
                    ("ui.mode", mode),
                    ("ui.alternate_screen", alternate),
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
