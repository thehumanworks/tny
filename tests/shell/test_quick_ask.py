#!/usr/bin/env python3
"""Exercise the real ZLE key binding in a PTY; no provider credentials needed."""

import json
import os
import select
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

try:
    import pty
except ImportError:
    pty = None

ROOT = Path(__file__).resolve().parents[2]
ZSH = shutil.which(os.environ.get("ZSH", "zsh"))
KEY = b"\x18a"
FAKE = """#!{python}
import json, os, signal, sys, time
from pathlib import Path
record = Path(os.environ["RECORD"])
with record.open("a") as f:
    f.write(json.dumps({{"args": sys.argv[1:], "prompt": sys.stdin.read(),
                        "cwd": os.getcwd()}}) + "\\n")
mode = Path(os.environ["MODE"]).read_text()
if mode == "wait":
    def stop(*args):
        # Ctrl-C may arrive while WAITING's buffered writer still holds its lock.
        os.write(1, b"INTERRUPTED\\n")
        os._exit(130)
    signal.signal(signal.SIGINT, stop)
    print("WAITING", flush=True)
    time.sleep(60)
print("ANSWER", flush=True)
sys.exit(2 if mode == "fail" else 0)
"""


class Shell:
    def __init__(self, directory, style="emacs", binary=None, flags=None):
        self.directory = directory
        self.record = directory / "record"
        self.mode = directory / "mode"
        self.mode.write_text("ok")
        fake = directory / "fake tny"
        fake.write_text(FAKE.format(python=sys.executable))
        fake.chmod(0o755)
        script = directory / ".zshrc"
        script.write_text(
            "PROMPT='READY> '; RPROMPT=''; PS2='MORE> '\n"
            "HISTFILE=$HOME/history; HISTSIZE=100; SAVEHIST=100\n"
            "setopt inc_append_history\n"
            f"bindkey {'-v' if style == 'vi' else '-e'}\n"
            f"TNY_BIN={shlex.quote(str(binary or fake))}\n"
            f"TNY_ASK_FLAGS=({shlex.join(flags or [])})\n"
            f"source {shlex.quote(str(ROOT / 'shell/tny.zsh'))}\n"
            f"source {shlex.quote(str(ROOT / 'shell/tny.zsh'))}\n"
        )
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(directory)
            os.environ.update(
                HOME=str(directory),
                ZDOTDIR=str(directory),
                TERM="xterm-256color",
                RECORD=str(self.record),
                MODE=str(self.mode),
            )
            os.execv(ZSH, [ZSH, "-d", "-i"])
        self.output = b""
        self.expect(b"READY> ")

    def send(self, data):
        os.write(self.fd, data)

    def expect(self, marker, timeout=10):
        deadline = time.monotonic() + timeout
        while marker not in self.output:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"missing {marker!r}: {self.output!r}")
            if select.select([self.fd], [], [], remaining)[0]:
                self.output += os.read(self.fd, 65536)
        end = self.output.index(marker) + len(marker)
        consumed, self.output = self.output[:end], self.output[end:]
        return consumed

    def paste(self, text):
        self.send(b"\x1b[200~" + text.encode() + b"\x1b[201~")

    def records(self):
        return [json.loads(line) for line in self.record.read_text().splitlines()]

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(self.pid, 0)
        os.close(self.fd)


@unittest.skipUnless(ZSH and pty, "Zsh and POSIX PTYs required")
class QuickAskTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tny quick ask ")
        self.directory = Path(self.temp.name).resolve()
        self.shell = None

    def tearDown(self):
        if self.shell:
            self.shell.close()
        self.temp.cleanup()

    def start(self, **kwargs):
        self.shell = Shell(self.directory, **kwargs)
        return self.shell

    def test_literal_multiline_flags_cwd_and_no_history(self):
        flags = ["--provider", "codex", "--model", "gpt-5.6-luna"]
        shell = self.start(flags=flags)
        prompt = "? What's $HOME? $(touch SHOULD_NOT_EXIST); `pwd` | cat > x\n--help * ! café\n"
        shell.paste(prompt)
        shell.send(KEY)
        shell.expect(b"ANSWER")
        shell.expect(b"READY> ")
        self.assertEqual(
            shell.records(),
            [
                {
                    "args": flags + ["ask", "--ephemeral", "--stdin"],
                    "prompt": prompt,
                    "cwd": str(self.directory),
                }
            ],
        )
        self.assertFalse((self.directory / "SHOULD_NOT_EXIST").exists())
        # Enter executes a normal command, and successful asks cleared BUFFER.
        shell.send(b"print SHELL_OK\r")
        shell.expect(b"\r\nSHELL_OK\r\n")
        shell.expect(b"READY> ")
        history_file = self.directory / "history"
        self.assertIn("print SHELL_OK", history_file.read_text())
        self.assertNotIn("What's", history_file.read_text())

    def test_vi_insert_and_command_keymaps(self):
        shell = self.start(style="vi")
        for mode in ("insert", "command"):
            shell.paste(f"question from {mode}")
            if mode == "command":
                shell.send(b"\x1b")
                time.sleep(0.1)
            shell.send(KEY)
            shell.expect(b"ANSWER")
            shell.expect(b"READY> ")
        self.assertEqual(len(shell.records()), 2)

    def test_failure_preserves_prompt_for_retry(self):
        shell = self.start()
        shell.mode.write_text("fail")
        shell.paste("please answer literally")
        shell.send(KEY)
        shell.expect(b"ask exited 2")
        shell.mode.write_text("ok")
        shell.send(KEY)
        shell.expect(b"ANSWER")
        shell.expect(b"READY> ")
        self.assertEqual(
            [r["prompt"] for r in shell.records()], ["please answer literally"] * 2
        )

    def test_failure_restores_cursor_and_key_sequence_can_be_typed_slowly(self):
        shell = self.start()
        shell.send(b"KEYTIMEOUT=1\r")
        shell.expect(b"READY> ")
        shell.mode.write_text("fail")
        shell.paste("abc")
        shell.send(b"\x02")  # Ctrl-B: cursor between b and c.
        shell.send(b"\x18")
        time.sleep(0.1)  # Longer than the user's 10 ms KEYTIMEOUT.
        shell.send(b"a")
        shell.expect(b"ask exited 2")
        shell.mode.write_text("ok")
        shell.send(b"X" + KEY)
        shell.expect(b"ANSWER")
        shell.expect(b"READY> ")
        self.assertEqual([r["prompt"] for r in shell.records()], ["abc", "abXc"])

    def test_ctrl_c_interrupts_and_shell_recovers(self):
        shell = self.start()
        shell.mode.write_text("wait")
        shell.paste("cancel this question")
        shell.send(KEY)
        shell.expect(b"WAITING")
        shell.send(b"\x03")
        shell.expect(b"INTERRUPTED")
        shell.expect(b"ask exited 130")
        # The diagnostic precedes the widget's return to ZLE. Sending another
        # Ctrl-C immediately can interrupt the widget again instead of clearing
        # the edit buffer; wait for that buffer's redisplay before cancelling it.
        shell.expect(b"cancel this question")
        shell.send(b"\x03\x0c")
        # ZLE may only clear the buffer after Ctrl-C. Explicitly request a
        # redraw with Ctrl-L and wait for it before typing the next command;
        # otherwise that command can race the interrupt and lose its first byte.
        shell.expect(b"READY> ")
        shell.send(b"print RECOVERED\r")
        shell.expect(b"\r\nRECOVERED\r\n")

    def test_empty_input_does_not_launch(self):
        shell = self.start()
        shell.send(KEY + b"print EMPTY_OK\r")
        shell.expect(b"\r\nEMPTY_OK\r\n")
        shell.expect(b"READY> ")
        shell.paste(" \t ")
        shell.send(KEY + b"print SPACE_OK\r")
        shell.expect(b"\r\nSPACE_OK\r\n")
        shell.expect(b"READY> ")
        self.assertFalse(shell.record.exists())

    def test_missing_binary_preserves_prompt(self):
        shell = self.start(binary=self.directory / "missing")
        shell.paste("retry me")
        shell.send(KEY)
        shell.expect(b"ask exited 127")
        self.assertFalse(shell.record.exists())

    def test_secondary_prompt_is_not_submitted(self):
        shell = self.start()
        shell.send(b"print 'unfinished\r")
        shell.expect(b"MORE> ")
        shell.send(KEY)
        shell.expect(b"cancel the unfinished shell command")
        self.assertFalse(shell.record.exists())

    def test_noninteractive_source_is_inert(self):
        result = subprocess.run(
            [
                ZSH,
                "-f",
                "-c",
                'source "$1"; (( ! $+functions[_tny_ask_widget] ))',
                "zsh",
                str(ROOT / "shell/tny.zsh"),
            ],
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
