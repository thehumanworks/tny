"""Shared trusted-code loader for the runtime host and development checks.

Loading executes imports and setup with the current user's full permissions.
This module deliberately does not discover entries or invoke event handlers.
"""

import asyncio
import contextlib
import importlib.util
import inspect
import itertools
import os
import sys
from dataclasses import dataclass
from types import ModuleType
from typing import Any, Optional, Tuple

from .api import ExtensionAPI, HandlerRegistration
from .capabilities import CapabilityView

_MODULE_IDS = itertools.count()


@dataclass(frozen=True)
class LoadedExtension:
    name: str
    entry: str
    registrations: Tuple[HandlerRegistration, ...]


def _entry_path(entry: str, cwd: Optional[str]) -> Tuple[str, bool]:
    expanded = os.path.expanduser(entry)
    if not os.path.isabs(expanded):
        expanded = os.path.join(cwd or os.getcwd(), expanded)
    resolved = os.path.abspath(expanded)
    if os.path.isdir(resolved):
        index = os.path.join(resolved, "index.py")
        if not os.path.isfile(index):
            raise FileNotFoundError(
                "extension directory has no index.py: %s" % resolved
            )
        return index, True
    if not os.path.isfile(resolved):
        raise FileNotFoundError("extension entry does not exist: %s" % resolved)
    # The C discovery layer passes the resolved index.py path, not its parent
    # directory.  Treat it as a package so relative imports keep working and
    # the extension name remains the directory name.
    return resolved, os.path.basename(resolved) == "index.py"


@contextlib.contextmanager
def _temporary_sys_path(path: str) -> Any:
    sys.path.insert(0, path)
    try:
        yield
    finally:
        try:
            sys.path.remove(path)
        except ValueError:
            pass


def _load_module(
    entry: str, ordinal: int, cwd: Optional[str]
) -> Tuple[ModuleType, str, str]:
    path, is_package = _entry_path(entry, cwd)
    extension_dir = os.path.dirname(path)
    extension_name = (
        os.path.basename(extension_dir)
        if is_package
        else os.path.splitext(os.path.basename(path))[0]
    )
    # Repeated checks can load same-named packages from different directories.
    # Give each load its own namespace so relative imports cannot reuse a prior
    # package's cached submodules. Public extension names/order stay unchanged.
    module_name = "_tny_extension_%d_%s_%d" % (
        ordinal,
        extension_name.replace("-", "_"),
        next(_MODULE_IDS),
    )
    if is_package:
        spec = importlib.util.spec_from_file_location(
            module_name, path, submodule_search_locations=[extension_dir]
        )
    else:
        spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise ImportError("cannot create import spec for extension: %s" % path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        with _temporary_sys_path(extension_dir):
            spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(module_name, None)
        raise
    return module, extension_name, path


def run_awaitable(value: Any) -> Any:
    if inspect.isawaitable(value):
        return asyncio.run(value)
    return value


def load_extension(
    entry: str,
    ordinal: int,
    cwd: Optional[str] = None,
    capabilities: Optional[CapabilityView] = None,
) -> LoadedExtension:
    """Import one explicit trusted entry and run setup, never its handlers."""

    with contextlib.redirect_stdout(sys.stderr):
        module, name, path = _load_module(entry, ordinal, cwd)
        setup = getattr(module, "setup", None)
        if not callable(setup):
            raise TypeError("extension must define callable setup(api): %s" % path)
        api = ExtensionAPI(name, capabilities)
        run_awaitable(setup(api))
    return LoadedExtension(name, path, api.registrations)
