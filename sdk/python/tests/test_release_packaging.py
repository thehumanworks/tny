"""Release metadata tests that never contact a package registry."""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

PACKAGE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE_ROOT))
try:
    from release_version import build_version, version_from_tag
finally:
    sys.path.pop(0)


class ReleaseVersionTests(unittest.TestCase):
    def test_stable_and_prerelease_tags_are_canonical(self) -> None:
        self.assertEqual(version_from_tag("v1.2.3"), "1.2.3")
        self.assertEqual(version_from_tag("v1.2.3-rc.4"), "1.2.3rc4")

    def test_noncanonical_or_unmappable_tags_fail(self) -> None:
        for value in (
            "1.2.3",
            "v01.2.3",
            "v1.2",
            "v1.2.3-preview.1",
            "v1.2.3+dirty",
        ):
            with self.subTest(value=value), self.assertRaisesRegex(
                ValueError, "invalid release tag"
            ):
                version_from_tag(value)

    def test_registry_build_requires_an_explicit_tag(self) -> None:
        with patch.dict(
            os.environ,
            {"TNY_REQUIRE_RELEASE_TAG": "1"},
            clear=True,
        ), self.assertRaisesRegex(ValueError, "TNY_RELEASE_TAG is required"):
            build_version()

    def test_runtime_version_is_not_independently_hardcoded(self) -> None:
        source = (PACKAGE_ROOT / "src" / "tny" / "__init__.py").read_text(
            encoding="utf-8"
        )
        self.assertIn('_distribution_version("tny")', source)
        self.assertNotIn('__version__ = "0.5.0a1"', source)


if __name__ == "__main__":
    unittest.main()
