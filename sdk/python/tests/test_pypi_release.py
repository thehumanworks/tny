"""Offline publication-state tests; no package registry is contacted."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "publish_pypi_release.py"
spec = importlib.util.spec_from_file_location("publish_pypi_release", SCRIPT)
assert spec is not None and spec.loader is not None
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


def expected() -> dict[str, dict[str, object]]:
    return {
        "tny-1.2.3-py3-none-macosx_13_0_arm64.whl": {"sha256": "a" * 64},
        "tny-1.2.3-py3-none-manylinux_2_34_aarch64.whl": {"sha256": "b" * 64},
        "tny-1.2.3-py3-none-manylinux_2_34_x86_64.whl": {"sha256": "c" * 64},
    }


def release(values: dict[str, dict[str, object]]) -> dict[str, object]:
    return {
        "urls": [
            {
                "filename": filename,
                "packagetype": "bdist_wheel",
                "digests": {"sha256": value["sha256"]},
                "url": f"https://files.invalid/{filename}",
            }
            for filename, value in values.items()
        ]
    }


class PyPIPublicationTests(unittest.TestCase):
    def test_absent_release_requires_publish(self) -> None:
        self.assertTrue(publisher.assess_release(None, expected()))

    def test_identical_rerun_skips_publish(self) -> None:
        values = expected()
        self.assertFalse(publisher.assess_release(release(values), values))

    def test_partial_release_fails_closed(self) -> None:
        values = expected()
        partial = dict(list(values.items())[:1])
        with self.assertRaisesRegex(ValueError, "partial"):
            publisher.assess_release(release(partial), values)

    def test_digest_mismatch_fails_closed(self) -> None:
        values = expected()
        remote = release(values)
        remote["urls"][0]["digests"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            publisher.assess_release(remote, values)

    def test_download_hash_is_verified_before_writing(self) -> None:
        values = expected()
        remote = release(values)
        with (
            tempfile.TemporaryDirectory() as directory,
            self.assertRaisesRegex(ValueError, "downloaded PyPI hash mismatch"),
        ):
            publisher.download_and_verify(
                remote, values, Path(directory), fetch_bytes=lambda _url: b"not-a-wheel"
            )
            self.assertEqual(list(Path(directory).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
