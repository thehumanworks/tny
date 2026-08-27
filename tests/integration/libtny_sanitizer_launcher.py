#!/usr/bin/env python3
"""Run the native sanitizer host against a hostile library-mode provider."""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
MOCK = ROOT / "tests/integration/mock_openai.py"


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    host = Path(sys.argv[1]).resolve(strict=True)
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="tny-native-sanitize-") as temporary:
        root = Path(temporary)
        workspace = root / "workspace"
        workspace.mkdir()
        forbidden = root / "must-not-exist"
        arguments = json.dumps({"command": f"touch {forbidden}"})
        mock = subprocess.Popen(
            [sys.executable, os.fspath(MOCK), str(port)], cwd=ROOT,
            env=dict(os.environ, MOCK_EXPECT_WIRE="responses",
                     MOCK_CUSTOM_TOOL="terminal",
                     MOCK_HALLUCINATED_ARGUMENTS=arguments,
                     MOCK_CONNECTION_CLOSE="1", MOCK_CHUNK_WIDTH="1048576"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            assert mock.stdout is not None
            if b"ready" not in mock.stdout.readline():
                return 3
            completed = subprocess.run(
                [os.fspath(host), os.fspath(workspace),
                 f"http://127.0.0.1:{port}/v1"],
                cwd=ROOT, capture_output=True, text=True, timeout=180,
                check=False,
            )
            if completed.returncode != 0 or completed.stderr or forbidden.exists():
                sys.stderr.write(completed.stderr)
                return completed.returncode or 4
            if completed.stdout != "libtny-sanitizer-host: lifecycle passed\n":
                return 5
            return 0
        finally:
            if mock.poll() is None:
                mock.terminate()
            mock.wait(timeout=5)
            if mock.stdout is not None:
                mock.stdout.close()
            if mock.stderr is not None:
                mock.stderr.close()


if __name__ == "__main__":
    raise SystemExit(main())
