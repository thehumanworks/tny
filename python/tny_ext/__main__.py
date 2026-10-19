"""Agent-friendly development commands: ``python -m tny_ext --help``."""

import argparse
import json
import pathlib
import sys
from typing import Any, Dict, Optional, Sequence

from .capabilities import CapabilityView, capability_view_from_dict
from .development import check_extensions, extension_catalog


def _read_capabilities(path: str) -> CapabilityView:
    with pathlib.Path(path).open(encoding="utf-8") as source:
        value = json.load(source)
    if isinstance(value, dict) and "extensions" in value:
        extensions = value["extensions"]
        value = extensions.get("capabilities") if isinstance(extensions, dict) else None
    if not isinstance(value, dict) or "schema_version" not in value:
        raise ValueError("expected a capability object or tny doctor --json output")
    if type(value["schema_version"]) is not int:
        raise ValueError("capabilities schema_version must be an integer")
    view = capability_view_from_dict(value)
    if view.schema_version != 1:
        raise ValueError(
            "unsupported capabilities schema major %s" % view.schema_version
        )
    return view


def _print_catalog(catalog: Dict[str, Any]) -> None:
    print("Events (field types are in --json; provider support is in tny doctor):")
    for item in catalog["events"]:
        names = ", ".join(field["name"] for field in item["fields"])
        print("  %s: %s(%s)" % (item["name"], item["python_type"], names))
    print("Actions:")
    for item in catalog["actions"]:
        names = ", ".join(field["name"] for field in item["fields"])
        print("  %s: %s(%s)" % (item["kind"], item["python_type"], names))


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Explore and check tny's existing pure-stdlib Python plugin API."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    catalog_parser = commands.add_parser(
        "catalog", help="list event/action types without loading any extensions"
    )
    catalog_parser.add_argument("--json", action="store_true", help="emit JSON")
    check_parser = commands.add_parser(
        "check",
        help="execute trusted setup and check registrations without invoking handlers",
        description=(
            "Execute imports and setup for explicitly selected trusted extensions. "
            "This is NOT static analysis or a sandbox: setup has full user permissions. "
            "The checker does not invoke registered handlers or start inference."
        ),
    )
    check_parser.add_argument(
        "entries", nargs="+", help="trusted .py files or index.py packages"
    )
    check_parser.add_argument("--json", action="store_true", help="emit JSON")
    check_parser.add_argument(
        "--capabilities",
        metavar="PATH",
        help="capability JSON or saved tny doctor --json output",
    )
    args = parser.parse_args(argv)
    if args.command == "catalog":
        catalog = extension_catalog()
        if args.json:
            print(json.dumps(catalog, ensure_ascii=False, sort_keys=True))
        else:
            _print_catalog(catalog)
        return 0
    capabilities = None
    if args.capabilities:
        try:
            capabilities = _read_capabilities(args.capabilities)
        except (OSError, ValueError) as error:
            parser.error("cannot read capabilities: %s" % error)
    print(
        "Checking trusted extensions: imports and setup execute with full user "
        "permissions, not in a sandbox. Registered handlers are not invoked.",
        file=sys.stderr,
    )
    report = check_extensions(args.entries, capabilities=capabilities)
    if args.json:
        print(json.dumps(report.to_dict(), ensure_ascii=False, sort_keys=True))
    else:
        for extension in report.extensions:
            print(
                "%s: %d handlers (%s)"
                % (extension.name, extension.handlers, ", ".join(extension.events))
            )
        for diagnostic in report.diagnostics:
            event = " [%s]" % diagnostic.event if diagnostic.event else ""
            print(
                "%s %s%s: %s: %s"
                % (
                    diagnostic.severity,
                    diagnostic.entry,
                    event,
                    diagnostic.code,
                    diagnostic.message,
                )
            )
        print("Check %s." % ("passed" if report.ok else "failed"))
    return 0 if report.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
