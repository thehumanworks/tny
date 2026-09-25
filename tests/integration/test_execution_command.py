#!/usr/bin/env python3
"""Fresh command guardian: real nested shell ownership after executor death."""

import json
import os
import signal
import subprocess
import sys
import time
import unittest

import test_execution_code_mode as fixture
from code_mode_fixture import lua_string


@unittest.skipIf(fixture.WASM, "native command guardian")
class ExecutionCommand(unittest.TestCase):
    setUp = fixture.ExecutionCodeMode.setUp
    close_server = fixture.ExecutionCodeMode.close_server
    start = fixture.ExecutionCodeMode.start
    run_code = fixture.ExecutionCodeMode.run_code

    def test_guardian_preserves_output_exit_and_nested_environment(self):
        command = (
            'printf \'%s|%s|%s|%s\' "$PWD" "$TNY_NESTED" '
            '"$TNY_NESTED_MODE" "$TNY_SELF_IMPROVE"; '
            "printf stderr-marker >&2; exit 27"
        )
        code = f"print(tools.call('terminal', {lua_string(json.dumps({'command': command}))}))"
        output = self.run_code(code)
        self.assertIn("exit code: 27", output)
        self.assertIn(f"{self.workspace}|1|yolo|0", output)
        self.assertIn("stderr-marker", output)

    def test_executor_death_reaps_live_shell_and_grandchild(self):
        command = (
            "printf '%s' $$ > shell.pid; sleep 30 & "
            "printf '%s' $! > grandchild.pid; wait"
        )
        code = f"print(tools.call('terminal', {lua_string(json.dumps({'command': command}))}))"
        process = self.start(code, arguments={"code": code, "timeout_ms": 30000})
        shell = grandchild = guardian = executor = None
        complete = False
        try:
            deadline = time.monotonic() + 6
            while time.monotonic() < deadline:
                if (self.workspace / "grandchild.pid").exists():
                    shell_text = (self.workspace / "shell.pid").read_text()
                    child_text = (self.workspace / "grandchild.pid").read_text()
                    if shell_text and child_text:
                        shell, grandchild = int(shell_text), int(child_text)
                        break
                time.sleep(0.02)
            self.assertIsNotNone(shell, "command did not start")
            rows = subprocess.run(
                ["ps", "-axo", "pid=,ppid=,args="],
                capture_output=True,
                text=True,
                check=True,
            ).stdout
            processes = {}
            for row in rows.splitlines():
                parts = row.strip().split(None, 2)
                if len(parts) == 3:
                    processes[int(parts[0])] = (int(parts[1]), parts[2])
            guardian = processes[shell][0]
            executor = processes[guardian][0]
            self.assertIn("--exec-command", processes[guardian][1])
            self.assertIn("--exec-server", processes[executor][1])
            self.assertEqual(processes[executor][0], process.pid)
            self.assertEqual(processes[grandchild][0], shell)
            os.kill(executor, signal.SIGKILL)
            stdout, stderr = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 0, (stdout, stderr))
            self.assertEqual(len(self.server.outputs), 1)
            self.assertIn("error", self.server.outputs[0].lower())
            deadline = time.monotonic() + 10
            remaining = {shell, grandchild}
            while remaining and time.monotonic() < deadline:
                for pid in list(remaining):
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        remaining.remove(pid)
                if remaining:
                    time.sleep(0.02)
            self.assertFalse(
                remaining, f"owned command processes survived: {remaining}"
            )
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            # Fixture-owned processes only; no production cleanup result is
            # inferred from this failure cleanup path.
            for pid in () if complete else (grandchild, shell, guardian):
                if pid:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
