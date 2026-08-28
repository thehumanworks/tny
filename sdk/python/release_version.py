"""Strict release-tag to PEP 440 version conversion for package builds."""

from __future__ import annotations

import os
import platform
import re

_TAG = re.compile(
    r"^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-(a|b|rc)\.(0|[1-9]\d*))?$"
)
_DEVELOPMENT_VERSION = "0.0.0.dev0"


def version_from_tag(tag: str) -> str:
    """Return the canonical PEP 440 version derived from one release tag."""
    match = _TAG.fullmatch(tag)
    if match is None:
        raise ValueError(
            f"invalid release tag {tag!r}; expected vMAJOR.MINOR.PATCH or "
            "vMAJOR.MINOR.PATCH-(a|b|rc).N with no build metadata"
        )
    base = ".".join(match.group(index) for index in range(1, 4))
    value = (
        base if match.group(4) is None else f"{base}{match.group(4)}{match.group(5)}"
    )
    # The accepted grammar is deliberately narrower than PEP 440: the stable
    # release and a/b/rc forms emitted here are already canonical, so build
    # metadata and normalizing aliases can never silently change registry identity.
    return value


def build_version() -> str:
    """Use an explicit tag in release builds and a non-publishable local version otherwise."""
    tag = os.environ.get("TNY_RELEASE_TAG")
    if tag:
        return version_from_tag(tag)
    if os.environ.get("TNY_REQUIRE_RELEASE_TAG") == "1":
        raise ValueError("TNY_RELEASE_TAG is required for a registry package build")
    return _DEVELOPMENT_VERSION


def single_arch_platform_tag(platform_tag: str, machine: str | None = None) -> str:
    """Name the one architecture the bundled libtny was built for.

    A universal2 interpreter (the GitHub macOS runners) makes setuptools emit
    ``macosx_13_0_universal2``, but libtny is single-arch and the release
    validator (scripts/validate_sdk_release.py) only accepts ``_arm64``.
    """
    machine = machine or platform.machine()
    if platform_tag.startswith("macosx_") and platform_tag.endswith(
        ("_universal2", "_universal", "_fat", "_intel")
    ):
        return platform_tag.rsplit("_", 1)[0] + "_" + machine
    return platform_tag
