#!/usr/bin/env python3
"""Keep CLI argv parsers and their rendered help flags in lockstep."""

from __future__ import annotations

import os
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = Path(os.environ.get("TNY", "build/tny"))
if not TNY.is_absolute():
    TNY = ROOT / TNY

# Commands that share an implementation deliberately share the parser entry.
# This map also makes unusual dispatch (status has a persistence-mode branch,
# acp lives outside src/cli/) explicit. test_parser_map_covers_dispatch makes a
# newly dispatched command fail until its parser is registered here.
COMMAND_PARSERS = {
    "acp": ("cmd_acp_server",),
    "ask": ("cmd_ask",),
    "ask-user": ("cmd_ask_user",),
    "backends": ("cmd_backends",),
    "cursor": ("cmd_cursor",),
    "doctor": ("cmd_doctor",),
    "edit": ("cmd_edit",),
    "login": ("cmd_login",),
    "image": ("cmd_image",),
    "logout": ("cmd_logout",),
    "mcp": ("cmd_mcp",),
    "models": ("cmd_models",),
    "permissions": ("cmd_permissions",),
    "provider": ("cmd_provider",),
    "providers": ("cmd_backends",),
    "resume": ("cmd_resume",),
    "session": ("cmd_session",),
    "sessions": ("cmd_sessions",),
    "task": ("cmd_task",),
    "tasks": ("cmd_tasks",),
    "setup": ("cmd_setup",),
    "status": ("cmd_status", "cmd_status_ephemeral"),
    "usage": ("cmd_usage",),
    "workspace": ("cmd_workspace",),
}

# Parsed but intentionally not printed as a concrete flag. cli_parse_globals
# accepts --resume-<id> as an open-ended compatibility spelling, so there is no
# finite token that help could enumerate; --resume <last|id> is canonical.
PARSED_WITHOUT_HELP = {"<global>": {"--resume-*"}}

# Help-only token from the ACP passthrough example
# `tny --provider acp --agent gemini -- --acp`; it belongs to the child agent,
# not tny. No other parser/help mismatch is allowlisted.
HELP_WITHOUT_PARSER = {"<global>": {"--acp"}}

SOURCE_PATHS = [
    ROOT / "src/main.c",
    ROOT / "src/cli/args.c",
    *sorted((ROOT / "src/cli").glob("cmd_*.c")),
    ROOT / "src/backends/acp/acp_server.c",
]

FUNCTION_START_RE = re.compile(
    r"(?m)^(?:static\s+)?(?:[A-Za-z_]\w*[\s*]+)+"
    r"(?P<name>[A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{"
)
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
STRCMP_FLAG_RE = re.compile(
    r'\bstrcmp\s*\(\s*[^,]+,\s*"(?P<flag>--?[A-Za-z0-9][A-Za-z0-9-]*|--)"\s*\)'
    r"\s*==\s*0"
)
PREFIX_FLAG_RE = re.compile(
    r'\bstr_starts\s*\(\s*[^,]+,\s*"(?P<flag>--[A-Za-z0-9][A-Za-z0-9-]*[=-])"\s*\)'
)
LONG_HELP_FLAG_RE = re.compile(r"(?<![\w-])--[A-Za-z0-9][A-Za-z0-9-]*(?:=[^\s,;)]+)?")
SHORT_HELP_FLAG_RE = re.compile(r"(?<![\w-])-[A-Za-z](?![\w-])")


def matching_brace(source: str, opening: int) -> int:
    """Return the matching C brace while ignoring strings and comments."""
    depth = 0
    state = "code"
    i = opening
    while i < len(source):
        char = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "/" and nxt == "/":
                state = "line_comment"
                i += 1
            elif char == "/" and nxt == "*":
                state = "block_comment"
                i += 1
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return i
        elif state in {"string", "char"}:
            if char == "\\":
                i += 1
            elif (state == "string" and char == '"') or (
                state == "char" and char == "'"
            ):
                state = "code"
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment" and char == "*" and nxt == "/":
            state = "code"
            i += 1
        i += 1
    raise AssertionError("unmatched function brace")


def function_index() -> dict[str, tuple[Path, str]]:
    functions: dict[str, tuple[Path, str]] = {}
    for path in SOURCE_PATHS:
        source = path.read_text(encoding="utf-8")
        for match in FUNCTION_START_RE.finditer(source):
            opening = source.find("{", match.start())
            closing = matching_brace(source, opening)
            functions[match.group("name")] = (path, source[opening + 1 : closing])
    return functions


FUNCTIONS = function_index()


def normalize_flag(flag: str) -> str | None:
    if flag == "--":
        return None
    if flag.endswith("="):
        return flag[:-1]
    if flag.endswith("-"):
        return f"{flag}*"
    if flag.startswith("--") and "=" in flag:
        return flag.split("=", 1)[0]
    return flag


def flags_in_functions(
    entrypoints: tuple[str, ...], *, follow_calls: bool = True
) -> set[str]:
    """Extract flag comparisons from entrypoints and their local call graph."""
    flags: set[str] = set()
    pending = list(entrypoints)
    visited: set[str] = set()
    while pending:
        name = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        if name not in FUNCTIONS:
            raise AssertionError(f"parser function {name} was not found")
        _path, body = FUNCTIONS[name]
        for regex in (STRCMP_FLAG_RE, PREFIX_FLAG_RE):
            for match in regex.finditer(body):
                flag = normalize_flag(match.group("flag"))
                if flag:
                    flags.add(flag)
        if follow_calls:
            pending.extend(call for call in CALL_RE.findall(body) if call in FUNCTIONS)
    return flags


def help_flags(text: str) -> set[str]:
    flags = {
        normalize_flag(match.group()) for match in LONG_HELP_FLAG_RE.finditer(text)
    }
    flags.update(match.group() for match in SHORT_HELP_FLAG_RE.finditer(text))
    return {flag for flag in flags if flag}


def run_help(*args: str) -> str:
    result = subprocess.run(
        [str(TNY), *args, "--help"] if args else [str(TNY), "--help"],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"{' '.join(result.args)} exited {result.returncode}: {result.stderr.strip()}"
        )
    return result.stdout


def dispatched_commands() -> set[str]:
    source = (ROOT / "src/main.c").read_text(encoding="utf-8")
    commands = set(re.findall(r'strcmp\(cmd,\s*"([a-z][a-z0-9-]*)"\)\s*==\s*0', source))
    commands.add("help")  # argv[1] fast path, intentionally outside cmd dispatch
    return commands


class HelpFlagAlignmentTest(unittest.TestCase):
    maxDiff = None

    def test_parser_map_covers_dispatch(self) -> None:
        self.assertEqual(dispatched_commands() - {"help"}, set(COMMAND_PARSERS))

    def test_global_flags_match_root_help(self) -> None:
        parsed = flags_in_functions(("main", "cli_parse_globals"), follow_calls=False)
        documented = help_flags(run_help())
        missing = parsed - documented - PARSED_WITHOUT_HELP.get("<global>", set())
        unparsed = documented - parsed - HELP_WITHOUT_PARSER.get("<global>", set())
        self.assertEqual(
            missing, set(), f"global parser flags absent from help: {sorted(missing)}"
        )
        self.assertEqual(
            unparsed, set(), f"root help flags absent from parser: {sorted(unparsed)}"
        )

    def test_subcommand_flags_match_help(self) -> None:
        global_flags = flags_in_functions(
            ("main", "cli_parse_globals"), follow_calls=False
        )
        for command, entrypoints in sorted(COMMAND_PARSERS.items()):
            with self.subTest(command=command):
                parsed = flags_in_functions(entrypoints)
                documented = help_flags(run_help(command))
                missing = parsed - documented - PARSED_WITHOUT_HELP.get(command, set())
                unparsed = (
                    documented
                    - parsed
                    - global_flags
                    - HELP_WITHOUT_PARSER.get(command, set())
                )
                self.assertEqual(
                    missing,
                    set(),
                    f"{command} parser flags absent from help: {sorted(missing)}",
                )
                self.assertEqual(
                    unparsed,
                    set(),
                    f"{command} help flags absent from parser: {sorted(unparsed)}",
                )

    def test_root_help_lists_every_subcommand(self) -> None:
        root_help = run_help()
        command_section = root_help.split("Commands:\n", 1)[1].split(
            "\nGlobal flags", 1
        )[0]
        missing = {
            command
            for command in dispatched_commands()
            if not re.search(
                rf"(?<![\w-]){re.escape(command)}(?![\w-])", command_section
            )
        }
        self.assertEqual(
            missing, set(), f"subcommands absent from root help: {sorted(missing)}"
        )


if __name__ == "__main__":
    unittest.main()
