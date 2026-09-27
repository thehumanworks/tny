#!/usr/bin/env python3
"""schemas/settings.schema.json: editor contract for settings keys.

Focused on the image_input map (docs/adr/0089): editors must accept the
documented shapes, reject the malformed ones, and never mistake the reserved
root key for a named OpenAI-compatible provider profile. This is editor-side
evidence only; the runtime parser is verified separately by
tests/integration/test_image_input.py and the C unit suite. Uses jsonschema
when it is installed, and always runs the stdlib pattern checks.

Accepts (and ignores) the binary path tests/integration/run.sh appends.
"""

from __future__ import annotations

import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = ROOT / "schemas/settings.schema.json"
SCHEMA = json.loads(SCHEMA_PATH.read_text())

try:  # optional: full-document validation when the library is available
    import jsonschema
except ImportError:  # pragma: no cover - depends on the environment
    jsonschema = None


DICTIONARY = json.loads((ROOT / "schemas/dictionary.schema.json").read_text())


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
