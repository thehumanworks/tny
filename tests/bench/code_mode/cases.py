"""Paired synthetic capability workflows; oracles never enter model prompts."""

from __future__ import annotations

import json
import random
from typing import Any

LANGUAGES = ("lua", "javascript", "python")
REPEATS = 3
VARIANTS = 3


def dump(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)


def tool(name: str, description: str, properties: dict[str, Any]) -> dict[str, Any]:
    return {
        "name": name,
        "description": description,
        "parameters": {
            "type": "object",
            "properties": properties,
            "required": list(properties),
            "additionalProperties": False,
        },
    }


STRING = {"type": "string"}
CATALOG = [
    tool("fs.read", "Read a virtual file; returns its raw text.", {"path": STRING}),
    tool(
        "fs.write",
        "Write virtual text; returns JSON {ok:true}.",
        {"path": STRING, "content": STRING},
    ),
    tool(
        "fs.list",
        "List virtual paths starting with prefix; returns a JSON string array.",
        {"prefix": STRING},
    ),
    tool(
        "api.page",
        "Get a page; returns JSON {items:[{id:string}],next:string|null}.",
        {"cursor": {"type": ["string", "null"]}},
    ),
    tool(
        "api.check",
        "Returns JSON {ok:boolean,retry?:boolean,value?:integer}.",
        {"id": STRING},
    ),
]

LANGUAGE_HELP = {
    "lua": """Write a Lua 5.4 script for Tny's actual bounded runtime. Available libraries:
base, table, string, math, utf8. No io, os, package, require, load, pcall/xpcall,
setmetatable/getmetatable, coroutine, or debug. JSON: json.decode(text),
json.encode(value), json.null. Decoded arrays preserve their array identity.
Use json.decode('[]') to construct a possibly empty array; {} encodes as an
empty object, while a dense nonempty integer-keyed table encodes as an array.
Array indices begin at 1. tools.call takes two STRINGS, not a Lua table.""",
    "javascript": """Write a synchronous JavaScript script for embedded QuickJS (ES2025), not Node
or a browser. No modules, require, process, filesystem, fetch or timers. Use
JSON.parse(text), JSON.stringify(value) and null. tools.call takes two STRINGS,
not an object. It is synchronous; no await or promises are needed. print(...) and
console.log(...) are available. This is a script, not a function: no top-level return.""",
    "python": """Write a Python 3.14 script for embedded CPython. The json module is already
bound: json.loads(text), json.dumps(value). tools and print are already bound.
No imports, open, eval, exec, filesystem or process APIs are provided. Standard
collection/number/string builtins, sorted, range, enumerate, zip, sum, any/all,
map, min/max, abs, isinstance, round, next and reversed are available.
tools.call takes two STRINGS, not a dict. It is synchronous. Use None for JSON null.""",
}
COMMON = """Generate code only in the requested JSON object {\"code\":\"...\"}. Do not call any
Codex tools, inspect files, or run commands while generating. The external test
harness will execute the code. Values of virtual input files are deliberately
unknown: read them via the typed capabilities at execution time. Never hardcode
fixture answers. Every capability call is tools.call(name, JSON_ARGUMENT_STRING)
and returns a string. tools.list() returns the catalog JSON string and
tools.describe(name) returns a schema JSON string or null/nil/None.
All effects must use these capabilities. No other capabilities exist.
Read every input needed by the task. Write the requested JSON value to
output.json via fs.write, preserving arrays, objects, booleans and nulls.
After all writes succeed, print exactly done (one line). No other output.
Maximum 64 nested calls, 64 KiB printed output, 256 KiB code and 2 seconds of
execution. Each evaluation starts with fresh virtual state. Do not generate
setup code for the already provided tools or JSON APIs.
"""

TASKS = [
    (
        "filter",
        "Read input.json containing {items:[{id:string,active?:boolean,score:integer}],minimum:integer}. Select items whose active is exactly true and score is at least minimum. Write their IDs in lexicographic ascending order as an array.",
    ),
    (
        "aggregate",
        "Read input.json, an array of {department:string,cents:integer,approved:boolean}. Sum cents by department for entries with approved exactly true. Write an object mapping each department with at least one approved entry to its integer total; omit other departments.",
    ),
    (
        "join",
        "Read users.json, an array of {id:string,name:string}, and tickets.json, an array of {id:string,owner:string}. In ticket input order, write an array of {id:ticket_id,owner:matching_user_name_or_null}. Preserve tickets with unknown owners.",
    ),
    (
        "pagination",
        "Start api.page with cursor null, follow next until next is null, including empty intermediate/final pages. Collect unique item IDs from every page and write them as a lexicographically sorted array.",
    ),
    (
        "batch_edit",
        "List paths with fs.list prefix src/. Read every listed file. Replace every literal FEATURE=false occurrence with FEATURE=true. Write only files whose content changed, and write the lexicographically sorted array of changed paths to output.json. Do not modify unchanged files or other paths.",
    ),
    (
        "json_shapes",
        "Read input.json, a JSON object with nested values. Change only its top-level status to the string ready. Write the complete object preserving every other value and shape exactly, including nested empty arrays/objects, null, false, zero and empty strings.",
    ),
    (
        "ndjson",
        "Read events.ndjson. Every nonblank line is a JSON object with a string level field. Ignore blank/whitespace-only lines. Count events by level and write an object mapping each encountered level to its integer count.",
    ),
    (
        "top_k",
        "Read input.json, an array of {id:string,score:integer}. Sort by score descending, breaking ties by id lexicographically ascending. Write the first three records, or all records if fewer than three, as an array.",
    ),
    (
        "bounded_retry",
        "Read input.json, an array of unique string IDs. For each ID call api.check at most three times: stop immediately when ok is true, or when a failure has retry other than true. Retry only explicit retry:true failures within that bound. Write an object mapping every ID to its successful integer value or null if it never succeeded. Never call an ID again after success.",
    ),
    (
        "literal_replace",
        "Read input.json containing {text:string,needle:nonempty_string,replacement:string}. Replace every nonoverlapping literal needle occurrence in text with replacement; needle is NOT a pattern or regular expression. Write {text:result,count:number_of_replacements}.",
    ),
    (
        "unicode",
        "Read input.json, an array of {text:string,take:nonnegative_integer}. For every entry in order, write {prefix:first_take_Unicode_scalar_values,length:total_Unicode_scalar_count}. Count scalar values, NOT UTF-8 bytes, UTF-16 code units or grapheme clusters. Inputs contain no unpaired surrogates.",
    ),
    (
        "dependency_order",
        "Read input.json, an object mapping task IDs to arrays of prerequisite IDs. The graph is acyclic and all prerequisites exist as keys. Repeatedly emit the lexicographically smallest remaining task whose prerequisites have all been emitted. Write the resulting full topological order as an array.",
    ),
]


def fixture(task: str, variant: int) -> dict[str, Any]:
    rng = random.Random(90210 + variant)
    ids = [f"item-{n}" for n in rng.sample(range(100, 999), 8)]
    files: dict[str, str] = {}
    raw: dict[str, Any] = {"files": files}
    expected: dict[str, Any] = {}
    required: list[dict[str, Any]] = []
    if task == "filter":
        items = [
            {"id": name, "active": i % 3 != 0, "score": rng.randrange(30)}
            for i, name in enumerate(ids)
        ]
        items.append({"id": "missing-active", "score": 100})
        minimum = (15, 999, 0)[variant]
        files["input.json"] = dump({"items": items, "minimum": minimum})
        answer = sorted(
            x["id"] for x in items if x.get("active") is True and x["score"] >= minimum
        )
    elif task == "aggregate":
        items = [
            {
                "department": ("eng", "ops", "sales")[i % 3],
                "cents": rng.randrange(-50, 200),
                "approved": variant != 1 and i % 4 != 0,
            }
            for i in range(17)
        ]
        files["input.json"] = dump(items)
        answer = {}
        for item in items:
            if item["approved"]:
                dept = item["department"]
                answer[dept] = answer.get(dept, 0) + item["cents"]
    elif task == "join":
        users = [{"id": ids[i], "name": ("Ana", "Zoë", "李")[i]} for i in range(3)]
        tickets = [{"id": ids[i], "owner": ids[(i + variant) % 5]} for i in range(6)]
        files.update({"users.json": dump(users), "tickets.json": dump(tickets)})
        names = {u["id"]: u["name"] for u in users}
        answer = [{"id": t["id"], "owner": names.get(t["owner"])} for t in tickets]
    elif task == "pagination":
        raw["pages"] = {
            "__first__": {"items": [{"id": ids[2]}, {"id": ids[0]}], "next": "second"},
            "second": {"items": [{"id": ids[0]}, {"id": ids[4]}], "next": "last"},
            "last": {"items": [], "next": None},
        }
        required = [
            {"name": "api.page", "args": {"cursor": c}}
            for c in (None, "second", "last")
        ]
        answer = sorted({ids[0], ids[2], ids[4]})
    elif task == "batch_edit":
        files.update(
            {
                "src/z.txt": "already FEATURE=true\n",
                "src/a.txt": f"{ids[0]} FEATURE=false FEATURE=false\n",
                "src/é.txt": "FEATURE=false\n",
                "README.md": "FEATURE=false, leave alone",
            }
        )
        answer = []
        for name, content in files.items():
            if name.startswith("src/") and "FEATURE=false" in content:
                expected[name] = content.replace("FEATURE=false", "FEATURE=true")
                answer.append(name)
        answer.sort()
        required.append({"name": "fs.list", "args": {"prefix": "src/"}})
    elif task == "json_shapes":
        value = {
            "items": [],
            "empty": {},
            "enabled": False,
            "count": 0,
            "optional": None,
            "nested": [[], {}, None, 0, False, "", {"array": [None, [], {}]}],
            "label": ids[variant],
            "status": "old",
        }
        files["input.json"] = dump(value)
        answer = {**value, "status": "ready"}
    elif task == "ndjson":
        levels = (
            []
            if variant == 1
            else [rng.choice(["info", "warn", "error"]) for _ in range(13)]
        )
        files["events.ndjson"] = (
            " \n"
            + "\n\n".join(
                dump({"level": level, "id": i}) for i, level in enumerate(levels)
            )
            + "\n\t\n"
        )
        answer = {level: levels.count(level) for level in set(levels)}
    elif task == "top_k":
        items = (
            []
            if variant == 1
            else [
                {"id": name, "score": (i + variant) % 3} for i, name in enumerate(ids)
            ]
        )
        files["input.json"] = dump(items)
        answer = sorted(items, key=lambda item: (-item["score"], item["id"]))[:3]
    elif task == "bounded_retry":
        files["input.json"] = dump(ids[:3])
        raw["checks"] = {
            ids[0]: [{"ok": True, "value": 10 + variant}],
            ids[1]: [{"ok": False, "retry": True}, {"ok": True, "value": 20 + variant}],
            ids[2]: [{"ok": False, "retry": True}] * 3,
        }
        answer = {ids[0]: 10 + variant, ids[1]: 20 + variant, ids[2]: None}
        required += [{"name": "api.check", "args": {"id": name}} for name in ids[:3]]
    elif task == "literal_replace":
        needle = ("a.b[0]+?", "%x.*", "🙂+")[variant]
        text = f"{needle} start {ids[0]} {needle}{needle} end"
        replacement = ("$1\\path", "100%", "λ")[variant]
        files["input.json"] = dump(
            {"text": text, "needle": needle, "replacement": replacement}
        )
        answer = {
            "text": text.replace(needle, replacement),
            "count": text.count(needle),
        }
    elif task == "unicode":
        items = [
            {"text": "A🙂é𝄞Z", "take": 3 + variant},
            {"text": "e\u0301李", "take": variant},
            {"text": "", "take": 2},
            {"text": ids[0], "take": 99},
        ]
        files["input.json"] = dump(items)
        answer = [
            {"prefix": item["text"][: item["take"]], "length": len(item["text"])}
            for item in items
        ]
    elif task == "dependency_order":
        graph = (
            {}
            if variant == 1
            else {
                ids[0]: [],
                ids[1]: [ids[0]],
                ids[2]: [],
                ids[3]: [ids[1], ids[2]],
                ids[4]: [ids[2]],
            }
        )
        files["input.json"] = dump(graph)
        answer = []
        while len(answer) < len(graph):
            answer.append(
                min(
                    key
                    for key, deps in graph.items()
                    if key not in answer and all(d in answer for d in deps)
                )
            )
    else:
        raise ValueError(task)
    required += [
        {"name": "fs.read", "args": {"path": name}}
        for name in files
        if task != "batch_edit" or name.startswith("src/")
    ]
    expected["output.json"] = answer
    return {
        "runtime": raw,
        "expected": expected,
        "required_calls": required,
        "json_paths": ["output.json"],
    }


def prompt(language: str, task: str) -> str:
    description = dict(TASKS)[task]
    return (
        COMMON
        + "\n"
        + LANGUAGE_HELP[language]
        + "\nTyped capability catalog:\n"
        + dump(CATALOG)
        + "\nTask:\n"
        + description
    )
