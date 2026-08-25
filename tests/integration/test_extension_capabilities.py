#!/usr/bin/env python3
"""Doctor capability discovery is exact, secret-free, and side-effect-free."""

import json
import os
import pathlib
import shlex
import stat
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
TNY = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", ROOT / "build" / "tny"))


def executable(path: pathlib.Path, marker: pathlib.Path) -> None:
    path.write_text(
        "#!/bin/sh\nprintf invoked >> %s\nexit 97\n" % shlex.quote(str(marker)),
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="tny-capability-doctor-") as root_value:
        root = pathlib.Path(root_value)
        home = root / "home"
        bin_dir = root / "bin"
        home.mkdir()
        bin_dir.mkdir()
        marker = root / "unexpected-spawn"
        for name in ("python3", "cursor-sdk-bridge", "codex"):
            executable(bin_dir / name, marker)

        secrets = (
            "DOCTOR_OPENAI_SECRET",
            "DOCTOR_CURSOR_SECRET",
            "DOCTOR_CODEX_SECRET",
        )
        env = dict(os.environ)
        env.update(
            {
                "HOME": str(home),
                "PATH": str(bin_dir),
                "OPENAI_API_KEY": secrets[0],
                "CURSOR_API_KEY": secrets[1],
                "CODEX_REMOTE_TOKEN": secrets[2],
                "OPENAI_BASE_URL": "http://127.0.0.1:1/v1",
                "TNY_EXTENSION_HOST": str(bin_dir / "python3"),
            }
        )
        startup = subprocess.run(
            [str(TNY), "--provider", "openai", "status", "--json"],
            cwd=str(ROOT),
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=True,
        )
        json.loads(startup.stdout)
        assert not marker.exists(), "extension-free startup executed a sentinel"

        completed = subprocess.run(
            [str(TNY), "--provider", "cursor", "doctor", "--json"],
            cwd=str(ROOT),
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=True,
        )
        assert not marker.exists(), "doctor executed a provider or Python sentinel"
        combined = completed.stdout + completed.stderr
        for secret in secrets:
            assert secret not in combined

        result = json.loads(completed.stdout)
        capabilities = result["extensions"]["capabilities"]
        assert capabilities["schema_version"] == 1
        assert capabilities["selected_provider"] == "cursor"
        assert set(capabilities["providers"]) == {"openai", "cursor", "codex", "acp"}
        for provider in capabilities["providers"].values():
            assert len(provider["entries"]) == 29
        cursor = capabilities["providers"]["cursor"]["entries"]
        assert cursor["extensions.permission.observe"] == {
            "state": "unsupported",
            "reason": "protocol_missing",
        }
        native = capabilities["providers"]["openai"]["entries"]
        assert native["extensions.permission.observe"]["state"] == "supported"
        assert native["extensions.prompt.transform"]["state"] == "unavailable"
        providers = {item["name"]: item for item in result["providers"]}
        assert "probe skipped" in providers["cursor"]["detail"]
        assert "probe skipped" in providers["codex"]["detail"]

    print("test_extension_capabilities: all assertions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
