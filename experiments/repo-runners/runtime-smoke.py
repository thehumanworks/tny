#!/usr/bin/env python3
"""Run fixture-only tny runtime checks: tny-runtime-smoke.py BINARY CHECKOUT."""

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("checkout", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    checkout = args.checkout.resolve()
    sys.dont_write_bytecode = True
    with tempfile.TemporaryDirectory(prefix="tny-runtime-smoke-") as isolated:
        # Only an allowlist reaches the test processes: no real credentials or
        # user config paths are inherited. Each test also creates its own home.
        environment = {
            "HOME": isolated,
            "PATH": os.defpath,
            "TMPDIR": isolated,
            "PYTHONDONTWRITEBYTECODE": "1",
            "TNY": str(binary),
            "NO_COLOR": "1",
        }
        os.environ.clear()
        os.environ.update(environment)
        os.chdir(isolated)
        for command in (["--help"], ["tool", "list", "--json"]):
            result = subprocess.run(
                [str(binary), *command], text=True, capture_output=True, timeout=30
            )
            if result.returncode:
                print(result.stdout + result.stderr, file=sys.stderr)
                return result.returncode
            if command[0] == "tool":
                names = [item["function"]["name"] for item in json.loads(result.stdout)]
                assert "read_file" in names and "write_file" in names, names
            print("PASS:", " ".join(command), flush=True)
        # Existing modules accept an optional positional binary on sys.argv.
        sys.argv = [sys.argv[0]]
        suite = unittest.TestSuite()
        selections = [
            ("test_tool_cli", None),
            (
                "test_tool_extensions",
                [
                    "test_code_mode_calls_extensions",
                    "test_module_commands_take_schema_flags",
                    "test_tool_call_results_and_errors",
                ],
            ),
        ]
        for name, selected in selections:
            path = checkout / "tests" / "integration" / (name + ".py")
            spec = importlib.util.spec_from_file_location(name, path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            if selected is None:
                suite.addTests(unittest.defaultTestLoader.loadTestsFromModule(module))
            else:
                for method in selected:
                    suite.addTest(module.ToolExtensionsTest(method))
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
