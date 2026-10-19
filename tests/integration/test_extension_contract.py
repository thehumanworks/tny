#!/usr/bin/env python3
"""Check the production capability API against its explicit compatibility fixture.

Historical competitor hook lists and documentation formatting are not runtime
contracts. Keep the fixture as an independent expected result, not generated
from the implementation under test.
"""

import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TNY = pathlib.Path(
    sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", ROOT / "build" / "tny")
)
CAPABILITY_FIXTURE = ROOT / "tests" / "fixtures" / "extension_capabilities.json"


def main() -> int:
    fixture = json.loads(CAPABILITY_FIXTURE.read_text(encoding="utf-8"))
    keys = fixture["capabilities"]
    assert keys and len(keys) == len(set(keys))

    env = dict(os.environ, TNY_EXTENSIONS="off")
    completed = subprocess.run(
        [str(TNY), "--provider", "openai", "doctor", "--json"],
        cwd=str(ROOT),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
        check=True,
    )
    capabilities = json.loads(completed.stdout)["extensions"]["capabilities"]
    assert capabilities["schema_version"] == fixture["schema_version"]
    assert set(capabilities["providers"]) == set(fixture["providers"]), (
        "provider set drift"
    )
    for name, provider in capabilities["providers"].items():
        assert set(provider["entries"]) == set(keys), (name, "capability set drift")
        expected = fixture["providers"][name]
        assert provider["runtime"] == expected["runtime"]
        assert len(expected["states"]) == len(keys)
        assert [provider["entries"][key]["state"] for key in keys] == expected[
            "states"
        ], (name, "capability state drift")
        for key, entry in provider["entries"].items():
            assert entry["state"] in {"supported", "unsupported", "unavailable"}
            if entry["state"] == "supported":
                bridge_only = name == "acp" and (
                    key.startswith("extensions.tool.")
                    or key.startswith("extensions.permission.")
                )
                assert entry["reason"] == (
                    "tny_mcp_bridge_only" if bridge_only else "implemented"
                )
            elif entry["state"] == "unavailable":
                assert entry["reason"] == "contracted_not_implemented"
            else:
                assert entry["reason"] == "provider_owned"

    print("test_extension_contract: all assertions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
