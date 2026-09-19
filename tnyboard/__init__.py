"""Standalone, local, cooperative ticket boards (Python 3.9+, POSIX)."""

from .core import handle
from .terminal import render

__all__ = ["handle", "render"]
