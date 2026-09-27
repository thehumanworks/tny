"""Held-out task families for paired Python-runtime generation trials.

Authored after, and deliberately different from, the twelve published PR #197
families (ADR 0178 asks reconsideration to use new held-out families). The
capability catalog, bubblewrap executor and typed scorer are the preserved
benchmark's. Oracles and fixture values never enter model prompts.
"""

from __future__ import annotations

import json
import random
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "code_mode"))

from cases import CATALOG, LANGUAGE_HELP, dump  # noqa: E402
from execute import score as typed_score  # noqa: E402
from execute import strict_json  # noqa: E402

REPEATS = 3
VARIANTS = 3

COMMON = """Generate code only in the requested JSON object {\"code\":\"...\"}. Do not call any
Codex tools, inspect files, or run commands while generating. The external test
harness will execute the code. Values of virtual input files are deliberately
unknown: read them via the typed capabilities at execution time. Never hardcode
fixture answers. Every capability call is tools.call(name, JSON_ARGUMENT_STRING)
and returns a string. tools.list() returns the catalog JSON string and
tools.describe(name) returns a schema JSON string or None.
All effects must use these capabilities. No other capabilities exist.
Read every input needed by the task. Write each requested output file via
fs.write exactly as specified. JSON outputs must preserve arrays, objects,
booleans and nulls. After all writes succeed, print exactly done (one line).
No other output. Maximum 64 nested calls, 64 KiB printed output, 256 KiB code
and 2 seconds of execution. Each evaluation starts with fresh virtual state.
Do not generate setup code for the already provided tools or JSON APIs.
"""

# The CPython arm's help is the proposed production run_code wording; the
# lighter arm adds only its runtime's documented, model-relevant restrictions.
HELP = {
    "cpython": """Write a Python 3.14 script. It runs in a fresh embedded CPython 3.14 interpreter.
json.loads(text) and json.dumps(value, ...) are pre-bound as json, and tools and
print are pre-bound. There is no import statement support, no open, eval, exec,
filesystem, network or process access: only tools reach the outside world.
Builtins, str/list/dict/set/tuple methods, comprehensions, generators, classes
and exceptions behave as in CPython. tools.call takes two STRINGS (a tool name
and a JSON object text), not a dict. It is synchronous. Use None for JSON null.""",
    # Exact PR #197 Python wording, executed by the preserved narrow adapter.
    "cpython_pr197": LANGUAGE_HELP["python"],
    "monty": """Write a Python 3.14 script. It runs in Monty, a restricted Python 3.14 subset.
json.loads(text) and json.dumps(value, ...) are pre-bound as json, and tools and
print are pre-bound. There is no import statement support, no open, eval, exec,
filesystem, network or process access: only tools reach the outside world.
Subset limits: no generator functions (yield), no del statement, no class
inheritance or user-defined exception classes, no match statement; generator
expressions are evaluated eagerly; bool is not usable as an int in arithmetic;
call string methods on instances (s.lower()), not as str.lower; callable,
issubclass and ascii are unavailable. Otherwise behavior matches CPython.
tools.call takes two STRINGS (a tool name and a JSON object text), not a dict.
It is synchronous. Use None for JSON null.""",
}

TASKS = [
    (
        "config_merge",
        "Read defaults.json and overrides.json, both JSON objects. Deep-merge overrides into defaults: when both values are objects, merge them recursively; an override value of null deletes that key; any other override value (including arrays, false, 0, empty strings, empty arrays and empty objects) replaces the default. Keys keep the defaults' order and keys that exist only in overrides are appended in the overrides' order. Write the merged object to output.json. Object key order is checked.",
    ),
    (
        "csv_rollup",
        "Read sales.csv. Its first line is the header region,amount,rep. Fields never contain commas or quotes. Ignore blank or whitespace-only lines. amount is an integer that may be negative. Write to output.json an array of {region,total,count} objects, one per region, sorted by total descending and then by region ascending.",
    ),
    (
        "latest_versions",
        "Read records.json, an array of {id:string,version:integer,data:any JSON value}. For each id keep the record with the highest version; when that highest version appears more than once keep its earliest record. Write to output.json the kept records, as unchanged whole objects, in the order in which each id first appears in the input.",
    ),
    (
        "log_errors",
        "Read app.log. Every nonblank line has the form 'TIMESTAMP LEVEL COMPONENT: MESSAGE'. TIMESTAMP, LEVEL and COMPONENT contain no spaces and COMPONENT contains no colon; MESSAGE may contain spaces and colons. Consider only lines whose LEVEL is exactly ERROR. Write to output.json an object mapping each such component, in order of its first ERROR line, to {count:number_of_its_ERROR_lines,first:MESSAGE_of_its_first_ERROR_line}. Object key order is checked.",
    ),
    (
        "tree_sizes",
        "List paths with fs.list prefix docs/ and read every listed file. Write to output.json a nested object for the directory tree below docs/: path components are separated by /, each directory is an object and each file maps to the number of Unicode scalar values in its content. Insert keys at every level in lexicographic order; object key order is checked.",
    ),
    (
        "validate_records",
        "Read records.json, an array of objects that each have a string id, and schema.json, {required:[field names],types:{field:type}} where type is one of string, integer, number, boolean, null, array, object. JSON booleans are neither integers nor numbers; every integer is also a number. For each record the problems are 'missing:FIELD' for each absent required field and 'type:FIELD' for each present field listed in types whose value has another JSON type. Write to output.json {valid:[ids of records without problems in input order],invalid:{id:problems sorted lexicographically}} where invalid lists records with problems in input order.",
    ),
    (
        "tag_frequency",
        "Start api.page with cursor null and follow next until it is null, including empty pages. Items are {id:string,tags:[string]}. The same item id can appear on several pages; use only its first occurrence. Write to output.json an object mapping each tag to the number of distinct items having it (a tag repeated within one item counts once), with keys ordered by count descending and then tag ascending. Object key order is checked.",
    ),
    (
        "markdown_toc",
        "Read README.md. Collect ATX headings of levels 1 to 3 (a line beginning with 1 to 3 '#' characters followed by a space) that are outside fenced code blocks; a line beginning with ``` toggles fencing. For each heading, in order, emit two spaces for each level above 1 followed by '- [TITLE](#SLUG)', where TITLE is the heading text with surrounding whitespace removed. SLUG is TITLE lowercased, with every character that is not a letter, digit, space or hyphen removed (letters and digits include non-ASCII ones) and then each space replaced by a hyphen; when that slug was already used, append -1 for the first repeat, -2 for the second, and so on. Write these lines, each ending in a newline, to toc.md (exact text). Write an empty toc.md when there are no headings.",
    ),
    (
        "interval_merge",
        "Read bookings.json, an array of {room:string,start:integer,end:integer} with start < end in arbitrary order. For each room merge bookings that overlap or touch (one ends exactly where another starts). Write to output.json an object mapping every room, with keys in lexicographic order, to its merged [start,end] pairs sorted by start. Object key order is checked.",
    ),
    (
        "json_diff",
        "Read before.json and after.json, both objects mapping string ids to arbitrary JSON values. Write to output.json {added:[ids only in after],removed:[ids only in before],changed:[ids in both whose values differ]}, each array sorted lexicographically. Compare as JSON values: booleans never equal numbers (true is not 1, false is not 0), integers and floats compare numerically (1 equals 1.0), null only equals null, object key order is irrelevant and array order matters.",
    ),
    (
        "order_rollup",
        "Read orders.json, an array of {customer:string,items:[{sku:string,qty:integer}]}; qty can be negative (returns). Write to output.json an object mapping each customer, in order of first appearance, to an object mapping each sku, in order of its first appearance for that customer, to the customer's total qty for it. Omit skus whose total is 0, and omit customers left with no skus. Object key order is checked.",
    ),
    (
        "template_render",
        "Read template.txt and values.json, an object mapping names to strings or integers. A placeholder is {{NAME}} where NAME is one or more ASCII letters, digits or underscores. Replace every placeholder whose NAME is a key of values with that value (strings unchanged, integers in decimal); leave every other placeholder unchanged. Write the result to output.txt (exact text) and write to report.json {replaced:number_of_replacements,missing:[unique unknown placeholder NAMEs sorted lexicographically]}.",
    ),
]

ORDERED = {"config_merge", "log_errors", "tree_sizes", "tag_frequency", "interval_merge", "order_rollup"}


def prompt(arm: str, task: str) -> str:
    return COMMON + "\n" + HELP[arm] + "\nTyped capability catalog:\n" + dump(CATALOG) + "\nTask:\n" + dict(TASKS)[task]


def _merge(base: dict[str, Any], over: dict[str, Any]) -> dict[str, Any]:
    out = dict(base)
    for key, value in over.items():
        if value is None:
            out.pop(key, None)
        elif isinstance(out.get(key), dict) and isinstance(value, dict):
            out[key] = _merge(out[key], value)
        else:
            out[key] = value
    return out


def _json_type(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    return "array" if isinstance(value, list) else "object"


def _json_equal(a: Any, b: Any) -> bool:
    if isinstance(a, bool) or isinstance(b, bool) or a is None or b is None:
        return type(a) is type(b) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(_json_equal(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(_json_equal(a[k], b[k]) for k in a)
    return type(a) is type(b) and a == b


def _slug(title: str) -> str:
    kept = "".join(c for c in title.lower() if c.isalnum() or c in " -")
    return kept.replace(" ", "-")


def fixture(task: str, variant: int) -> dict[str, Any]:
    rng = random.Random(4242 + 17 * variant)
    ids = [f"id-{n}" for n in rng.sample(range(100, 999), 10)]
    files: dict[str, str] = {}
    raw: dict[str, Any] = {"files": files}
    expected: dict[str, Any] = {}
    required: list[dict[str, Any]] = []
    json_paths = ["output.json"]
    if task == "config_merge":
        defaults = {
            "name": "svc",
            "port": 8080,
            "features": {"a": True, "b": False, "nested": {"x": 1, "y": [1, 2]}},
            "tags": ["x"],
            "limits": {},
            "debug": False,
        }
        overrides = [
            {"port": 0, "features": {"b": None, "nested": {"y": [], "z": None}, "c": ""}, "extra": {}, "tags": None},
            {},
            {"features": None, "name": "svc2", "debug": None, "limits": {"cpu": 2}, "new": [None, False]},
        ][variant]
        files["defaults.json"] = dump(defaults)
        files["overrides.json"] = dump(overrides)
        answer: Any = _merge(defaults, overrides)
    elif task == "csv_rollup":
        regions = ["north", "south", "east", "west", "zoë"]
        lines = ["region,amount,rep"]
        for i in range(0 if variant == 1 else 14):
            lines.append(f"{rng.choice(regions)},{rng.randrange(-40, 120)},{ids[i % 10]}")
            if i % 5 == 2:
                lines.append("   ")
        files["sales.csv"] = "\n".join(lines) + "\n"
        totals: dict[str, list[int]] = {}
        for line in lines[1:]:
            if line.strip():
                region, amount, _ = line.split(",")
                bucket = totals.setdefault(region, [0, 0])
                bucket[0] += int(amount)
                bucket[1] += 1
        answer = [
            {"region": r, "total": t, "count": c}
            for r, (t, c) in sorted(totals.items(), key=lambda kv: (-kv[1][0], kv[0]))
        ]
    elif task == "latest_versions":
        records = []
        for i in range(0 if variant == 1 else 12):
            records.append(
                {"id": ids[i % 4], "version": rng.randrange(1, 4), "data": [None, {"n": i}, False, 0, "", []][i % 6]}
            )
        files["records.json"] = dump(records)
        kept: dict[str, dict[str, Any]] = {}
        for record in records:
            best = kept.get(record["id"])
            if best is None or record["version"] > best["version"]:
                kept[record["id"]] = record
        answer = list(kept.values())
    elif task == "log_errors":
        components = ["db", "api", "auth", "cache"]
        levels = ["INFO", "ERROR", "WARN", "ERROR", "ERRORS", "error"]
        lines = []
        for i in range(0 if variant == 1 else 16):
            component = components[(i * (variant + 1)) % 4]
            message = f"failed: code={i}: retry {ids[i % 10]}"
            lines.append(f"2026-09-{10 + i}T0{i % 10}:00:00 {levels[i % 6]} {component}: {message}")
            if i % 7 == 3:
                lines.append("")
        files["app.log"] = "\n".join(lines) + "\n"
        answer = {}
        for line in lines:
            if not line.strip():
                continue
            _, level, rest = line.split(" ", 2)
            if level != "ERROR":
                continue
            component, message = rest.split(": ", 1)
            entry = answer.setdefault(component, {"count": 0, "first": message})
            entry["count"] += 1
    elif task == "tree_sizes":
        paths = [
            ["docs/guide/intro.md", "docs/a.md", "docs/guide/advanced/xé.md", "docs/api/z.md", "docs/api/b.md"],
            ["docs/only.md"],
            ["docs/b/c/d/e.txt", "docs/b/a.txt", "docs/å.md"],
        ][variant]
        for i, path in enumerate(paths):
            files[path] = ("\U0001f642 é " * (i + 1)) + ids[i]
        files["other/skip.md"] = "not under docs"
        tree: dict[str, Any] = {}
        for path in sorted(paths):
            node = tree
            parts = path[len("docs/") :].split("/")
            for part in parts[:-1]:
                node = node.setdefault(part, {})
            node[parts[-1]] = len(files[path])

        def ordered(node: dict[str, Any]) -> dict[str, Any]:
            return {k: ordered(v) if isinstance(v, dict) else v for k, v in sorted(node.items())}

        answer = ordered(tree)
        required.append({"name": "fs.list", "args": {"prefix": "docs/"}})
    elif task == "validate_records":
        schema = {
            "required": ["id", "count", "active"],
            "types": {"count": "integer", "ratio": "number", "active": "boolean", "tags": "array", "meta": "object", "note": "null"},
        }
        values = [
            {"count": 3, "active": True, "ratio": 1},
            {"count": True, "active": 1, "ratio": 2.5},
            {"active": False, "tags": {}, "meta": []},
            {"count": 0, "active": False, "note": None, "tags": [], "meta": {}},
            {"count": 2.5, "active": None, "ratio": "1"},
            {"count": -7, "active": True, "note": 0},
        ]
        records = [{"id": ids[i], **values[(i + variant) % 6]} for i in range(0 if variant == 1 else 6)]
        files["records.json"] = dump(records)
        files["schema.json"] = dump(schema)
        valid, invalid = [], {}
        for record in records:
            problems = [f"missing:{f}" for f in schema["required"] if f not in record]
            for field, kind in schema["types"].items():
                if field in record:
                    actual = _json_type(record[field])
                    ok = actual == kind or (kind == "number" and actual == "integer")
                    if not ok:
                        problems.append(f"type:{field}")
            if problems:
                invalid[record["id"]] = sorted(problems)
            else:
                valid.append(record["id"])
        answer = {"valid": valid, "invalid": invalid}
    elif task == "tag_frequency":
        tags = ["red", "blue", "green", "été"]
        pages: dict[str, Any] = {}
        cursors = ["__first__", "p2", "p3", "p4"]
        seen: dict[str, list[str]] = {}
        for index, cursor in enumerate(cursors):
            items = []
            for j in range(0 if (variant == 1 or index == 2) else 3):
                item_id = ids[(index * 2 + j) % 7]
                item_tags = [tags[(index + j + k) % 4] for k in range(1 + (j + variant) % 3)] + ([tags[0]] if j == 1 else [])
                items.append({"id": item_id, "tags": item_tags})
                seen.setdefault(item_id, item_tags)
            pages[cursor] = {"items": items, "next": cursors[index + 1] if index < 3 else None}
        raw["pages"] = pages
        counts: dict[str, int] = {}
        for item_tags in seen.values():
            for tag in set(item_tags):
                counts[tag] = counts.get(tag, 0) + 1
        answer = dict(sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])))
        required = [{"name": "api.page", "args": {"cursor": c}} for c in [None, "p2", "p3", "p4"]]
    elif task == "markdown_toc":
        docs = [
            "# Tny Guide\nintro\n## Install & Run\n```sh\n# not a heading\n```\n### Café notes\n#### too deep\n## Install & Run\n#nospace\n  ## indented\n## API: v2 (beta)\n## Install & Run\n",
            "no headings here\n```\n# fenced\n```\n",
            "## Старт — go!\n# A  B\n```\n## hidden\n```\n### x_y-z\n# A  B\n",
        ]
        text = docs[variant]
        files["README.md"] = text
        out_lines, used, fenced = [], {}, False
        for line in text.split("\n"):
            if line.startswith("```"):
                fenced = not fenced
                continue
            if fenced:
                continue
            level = len(line) - len(line.lstrip("#"))
            if 1 <= level <= 3 and line[level : level + 1] == " ":
                title = line[level:].strip()
                slug = _slug(title)
                if slug in used:
                    used[slug] += 1
                    slug = f"{slug}-{used[slug]}"
                else:
                    used[slug] = 0
                out_lines.append("  " * (level - 1) + f"- [{title}](#{slug})")
        expected["toc.md"] = "".join(line + "\n" for line in out_lines)
        json_paths = []
        answer = None
    elif task == "interval_merge":
        bookings = []
        for i in range(0 if variant == 1 else 12):
            start = rng.randrange(0, 40)
            bookings.append({"room": ["b", "a", "é", "c"][i % (2 + variant)], "start": start, "end": start + rng.randrange(1, 9)})
        if variant == 2:
            bookings += [{"room": "a", "start": 50, "end": 55}, {"room": "a", "start": 55, "end": 60}]
        files["bookings.json"] = dump(bookings)
        rooms: dict[str, list[list[int]]] = {}
        for booking in sorted(bookings, key=lambda b: (b["room"], b["start"], b["end"])):
            spans = rooms.setdefault(booking["room"], [])
            if spans and booking["start"] <= spans[-1][1]:
                spans[-1][1] = max(spans[-1][1], booking["end"])
            else:
                spans.append([booking["start"], booking["end"]])
        answer = dict(sorted(rooms.items()))
    elif task == "json_diff":
        before = {ids[0]: 1, ids[1]: False, ids[2]: {"a": 1, "b": [1, 2]}, ids[3]: None, ids[4]: "x", ids[5]: [1, {}]}
        afters = [
            {ids[0]: 1.0, ids[1]: 0, ids[2]: {"b": [1, 2], "a": 1}, ids[3]: None, ids[6]: True, ids[5]: [{}, 1]},
            dict(before),
            {ids[0]: True, ids[2]: {"a": 1, "b": [1, 2.0]}, ids[3]: False, ids[4]: "x ", ids[7]: None},
        ]
        after = afters[variant]
        files["before.json"] = dump(before)
        files["after.json"] = dump(after)
        answer = {
            "added": sorted(k for k in after if k not in before),
            "removed": sorted(k for k in before if k not in after),
            "changed": sorted(k for k in before if k in after and not _json_equal(before[k], after[k])),
        }
    elif task == "order_rollup":
        orders = []
        for i in range(0 if variant == 1 else 7):
            items = [{"sku": f"sku-{(i + j * (variant + 2)) % 5}", "qty": (j * 3 + i) % 7 - 3} for j in range(1 + i % 3)]
            orders.append({"customer": ["zed", "amy", "öz", "amy"][i % 4], "items": items})
        files["orders.json"] = dump(orders)
        totals2: dict[str, dict[str, int]] = {}
        for order in orders:
            per = totals2.setdefault(order["customer"], {})
            for item in order["items"]:
                per[item["sku"]] = per.get(item["sku"], 0) + item["qty"]
        answer = {}
        for customer, per in totals2.items():
            kept2 = {sku: qty for sku, qty in per.items() if qty != 0}
            if kept2:
                answer[customer] = kept2
    elif task == "template_render":
        templates = [
            "Hi {{name}}, you owe {{amount}} {{currency}}. {{name}}! {{ name }} {{x-y}} {{missing_1}} {{}} {{{name}}} {{unknown}}{{missing_1}}\n",
            "no placeholders {name} {{\n",
            "{{a}}{{b}}{{a_b}}{{A}} café {{é}} {{b}}",
        ]
        values = [{"name": "Zoë", "amount": 42, "currency": "EUR", "unused": "x"}, {"name": "x"}, {"a": "", "b": -7, "A": "{{a}}"}][variant]
        template = templates[variant]
        files["template.txt"] = template
        files["values.json"] = dump(values)
        out, i, replaced, missing = [], 0, 0, set()
        while i < len(template):
            if template.startswith("{{", i):
                end = template.find("}}", i + 2)
                name = template[i + 2 : end] if end >= 0 else ""
                if end >= 0 and name and all(c.isascii() and (c.isalnum() or c == "_") for c in name):
                    if name in values:
                        out.append(str(values[name]))
                        replaced += 1
                    else:
                        out.append(template[i : end + 2])
                        missing.add(name)
                    i = end + 2
                    continue
            out.append(template[i])
            i += 1
        expected["output.txt"] = "".join(out)
        expected["report.json"] = {"replaced": replaced, "missing": sorted(missing)}
        json_paths = ["report.json"]
        answer = None
    else:
        raise ValueError(task)
    required += [{"name": "fs.read", "args": {"path": name}} for name in files if not name.startswith("other/")]
    if answer is not None:
        expected["output.json"] = answer
    return {"runtime": raw, "expected": expected, "required_calls": required, "json_paths": json_paths}


def _pairs(value: Any) -> Any:
    if isinstance(value, dict):
        return [[k, _pairs(v)] for k, v in value.items()]
    if isinstance(value, list):
        return [_pairs(v) for v in value]
    return value


def score(task: str, result: dict[str, Any], case: dict[str, Any]) -> dict[str, Any]:
    """Preserved typed scorer, plus exact key order where the task demands it."""
    scored = typed_score(result, case)
    if task in ORDERED and scored["output_ok"]:
        observed = strict_json(result["writes"]["output.json"])
        order_ok = _pairs(observed) == _pairs(case["expected"]["output.json"])
        scored["output_ok"] = order_ok
        scored["passed"] = scored["passed"] and order_ok
    return scored


if __name__ == "__main__":
    for name, _ in TASKS:
        for v in range(VARIANTS):
            case = fixture(name, v)
            print(name, v, json.dumps(case["expected"], ensure_ascii=False)[:160])
