#!/usr/bin/env python3
"""Fresh command guardian: real nested shell ownership after executor death."""

import ctypes
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

import test_execution_code_mode as fixture
from code_mode_fixture import python_string


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
        code = f"print(tools.call('terminal', {python_string(json.dumps({'command': command}))}))"
        output = self.run_code(code)
        self.assertIn("exit code: 27", output)
        self.assertIn(f"{self.workspace}|1|yolo|0", output)
        self.assertIn("stderr-marker", output)

    @unittest.skipUnless(sys.platform == "linux", "Linux owned checks")
    def test_terminal_owned_check_consumer(self):
        arguments = {
            "command": "sleep 30 >/dev/null 2>&1 & exit 0",
            "owned_check": True,
        }
        code = f"print(tools.call('terminal', {python_string(json.dumps(arguments))}))"
        output = self.run_code(code)
        self.assertIn("exit code: 125", output)
        self.assertIn("owned cleanup: unconfirmed", output)

    @unittest.skipUnless(sys.platform == "linux", "Linux owned checks")
    def test_terminal_owned_check_clean_or_unsupported(self):
        supported = Path(f"/proc/self/task/{os.getpid()}/children").exists()
        arguments = {"command": "printf owned-check-ran", "owned_check": True}
        code = f"print(tools.call('terminal', {python_string(json.dumps(arguments))}))"
        output = self.run_code(code)
        self.assertIn("exit code: 0" if supported else "exit code: 125", output)
        if supported:
            self.assertIn("owned cleanup: complete", output)
            self.assertIn("owned-check-ran", output)
        else:
            self.assertNotIn("owned-check-ran", output)

    @unittest.skipUnless(sys.platform == "linux", "Linux owned checks")
    def test_terminal_guardian_crash_never_reports_success(self):
        if not Path(f"/proc/self/task/{os.getpid()}/children").exists():
            if os.environ.get("CI"):
                self.fail("Linux CI requires owned check procfs")
            self.skipTest("owned check procfs unavailable")
        import shlex

        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        # This fixture has no descendants. Pin its own guardian identity before
        # injecting a crash, then exit immediately; the test reaps the orphan.
        payload = (
            "import os,signal; p=os.getppid(); assert p>1; "
            "fd=os.pidfd_open(p); assert os.getppid()==p; "
            "open('crash-root.pid','w').write(str(os.getpid())); "
            "signal.pidfd_send_signal(fd,signal.SIGKILL); os._exit(0)"
        )
        arguments = {
            "command": f"exec {shlex.quote(sys.executable)} -c {shlex.quote(payload)}",
            "owned_check": True,
        }
        try:
            code = (
                f"print(tools.call('terminal', {python_string(json.dumps(arguments))}))"
            )
            output = self.run_code(code)
            self.assertIn("exit code: 137", output)
            self.assertIn("owned cleanup: unconfirmed", output)
            self.assertNotIn("owned cleanup: complete", output)
        finally:
            try:
                marker = self.workspace / "crash-root.pid"
                if marker.exists():
                    pid = int(marker.read_text())
                    deadline = time.monotonic() + 2
                    while True:
                        try:
                            reaped = os.waitpid(pid, os.WNOHANG)[0]
                        except ChildProcessError:
                            # The dying guardian may have consumed the root's
                            # status before SIGKILL took effect. Never signal it.
                            self.assertFalse(Path(f"/proc/{pid}").exists())
                            reaped = pid
                        if reaped or time.monotonic() >= deadline:
                            break
                        time.sleep(0.01)
                    self.assertEqual(reaped, pid, "crash fixture root did not exit")
            finally:
                self.assertEqual(libc.prctl(36, previous.value, 0, 0, 0), 0)

    @unittest.skipIf(sys.platform == "linux", "unsupported native hosts only")
    def test_terminal_owned_check_unsupported_host(self):
        arguments = {"command": "touch must-not-run", "owned_check": True}
        code = f"print(tools.call('terminal', {python_string(json.dumps(arguments))}))"
        output = self.run_code(code)
        self.assertIn("supported only on native Linux", output)
        self.assertFalse((self.workspace / "must-not-run").exists())

    def test_executor_death_reaps_live_shell_and_grandchild(self):
        command = (
            "printf '%s' $$ > shell.pid; sleep 30 & "
            "printf '%s' $! > grandchild.pid; wait"
        )
        code = f"print(tools.call('terminal', {python_string(json.dumps({'command': command}))}))"
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


@unittest.skipUnless(sys.platform == "linux" and not fixture.WASM, "Linux owned checks")
class OwnedCommand(unittest.TestCase):
    def test_real_process_ownership(self):
        try:
            Path(f"/proc/self/task/{os.getpid()}/children").read_text()
        except OSError as error:
            if os.environ.get("CI"):
                self.fail(f"Linux CI requires owned check procfs: {error}")
            self.skipTest(f"owned check procfs unavailable: {error}")
        # Test-only subreaper permits exact fixture cleanup on a regression;
        # the production guardian must itself reap all of its descendants.
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        unrelated = subprocess.Popen(["/bin/sleep", "30"])
        try:
            for mode in (
                "normal",
                "nonzero",
                "zombie",
                "orphan",
                "nested",
                "timeout",
                "cancel",
                "lifeline",
            ):
                with (
                    self.subTest(mode=mode),
                    tempfile.TemporaryDirectory() as directory,
                ):
                    self.run_guardian(Path(directory), mode)
                    self.assertIsNone(
                        unrelated.poll(), "guardian killed unrelated sibling"
                    )
        finally:
            unrelated.kill()
            unrelated.wait(timeout=5)
            self.assertEqual(libc.prctl(36, previous.value, 0, 0, 0), 0)

    def run_guardian(self, directory, mode):
        parent, child = socket.socketpair()

        launcher = (
            "import os,sys; os.dup2(int(sys.argv[1]),3); "
            "os.set_inheritable(3,True); os.execv(sys.argv[2],[sys.argv[2],'--exec-command'])"
        )
        process = subprocess.Popen(
            [sys.executable, "-c", launcher, str(child.fileno()), fixture.TNY],
            pass_fds=(child.fileno(),),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        child.close()
        command = "printf clean; exit 0"
        if mode not in ("normal", "nonzero", "zombie"):
            command = "sleep 30 >/dev/null 2>&1 & echo $! > child.pid; "
            command += "exit 0" if mode == "orphan" else "wait"
        if mode == "nonzero":
            command = "exit 37"
        if mode == "zombie":
            import shlex

            code = (
                "import os,time; p=os.fork(); "
                "os._exit(0) if p==0 else None; "
                "open('child.pid','w').write(str(p)); time.sleep(0.1); os._exit(37)"
            )
            command = f"exec {shlex.quote(sys.executable)} -c {shlex.quote(code)}"
        if mode == "nested":
            # A nested subreaper and new session must not escape adoption after
            # its parent is killed. This is a cooperative fixture, not an attack.
            code = (
                "import ctypes,os,time; assert ctypes.CDLL(None).prctl(36,1,0,0,0)==0; "
                "os.setsid(); p=os.fork(); "
                "open('leaf.pid','w').write(str(os.getpid())) if p==0 else "
                "open('child.pid','w').write(str(os.getpid())); time.sleep(30)"
            )
            import shlex

            command = f"{shlex.quote(sys.executable)} -c {shlex.quote(code)} & wait"
        request = {
            "argv": ["/bin/sh", "-c", command],
            "cwd": str(directory),
            "timeout_ms": 250 if mode in ("timeout", "nested") else 5000,
            "session_sock": None,
            "session_id": None,
            "permission_mode": "yolo",
            "self_improve": False,
            "owned_check": True,
        }
        wire = json.dumps(request).encode()
        parent.sendall(struct.pack("!I", len(wire)) + wire)
        try:
            if mode in ("cancel", "lifeline"):
                deadline = time.monotonic() + 3
                while (
                    not (directory / "child.pid").exists()
                    and time.monotonic() < deadline
                ):
                    time.sleep(0.01)
                self.assertTrue((directory / "child.pid").exists())
                if mode == "cancel":
                    process.send_signal(signal.SIGTERM)
                else:
                    parent.close()
            stdout, stderr = process.communicate(timeout=8)
            expected = {"normal": 0, "nonzero": 37, "zombie": 37, "orphan": 125}.get(
                mode, 130
            )
            self.assertEqual(process.returncode, expected, (stdout, stderr))
            if mode not in ("normal", "nonzero"):
                self.assertTrue((directory / "child.pid").exists(), "command never ran")
            if mode == "nested":
                self.assertTrue(
                    (directory / "leaf.pid").exists(), "nested leaf never ran"
                )
            for path in directory.glob("*.pid"):
                pid = int(path.read_text())
                self.assertFalse(
                    Path(f"/proc/{pid}").exists(), f"unreaped descendant {pid}"
                )
        finally:
            parent.close()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            for path in directory.glob("*.pid"):
                # Only a currently adopted fixture child may be signalled;
                # WNOWAIT retains its identity until our explicit reap.
                pid = int(path.read_text())
                try:
                    os.waitid(os.P_PID, pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
                    os.kill(pid, signal.SIGKILL)
                    os.waitpid(pid, 0)
                except (ChildProcessError, ProcessLookupError):
                    pass


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
