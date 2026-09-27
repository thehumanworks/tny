"""Handwritten held-out controls, run on every arm before live inference.

Written within the common subset of every arm (no yield/del/inheritance/match,
no unbound str methods, no bool arithmetic, only the PR #197 builtin whitelist)
so a control failure indicates a fixture or executor defect, not a documented
restriction of one arm.
"""

PRELUDE = """
def read(path):
    return tools.call("fs.read", json.dumps({"path": path}))
def write(path, text):
    reply = json.loads(tools.call("fs.write", json.dumps({"path": path, "content": text})))
    if reply.get("ok") is not True:
        raise ValueError("write failed")
"""

REFERENCES = {
    "config_merge": """
def merge(base, over):
    out = {}
    for key in base:
        out[key] = base[key]
    for key in over:
        value = over[key]
        if value is None:
            if key in out:
                out = {k: v for k, v in out.items() if k != key}
        elif isinstance(out.get(key), dict) and isinstance(value, dict):
            out[key] = merge(out[key], value)
        else:
            out[key] = value
    return out
merged = merge(json.loads(read("defaults.json")), json.loads(read("overrides.json")))
write("output.json", json.dumps(merged))
print("done")
""",
    "csv_rollup": """
totals = {}
lines = read("sales.csv").split("\\n")
for line in lines[1:]:
    if not line.strip():
        continue
    region, amount, rep = line.split(",")
    entry = totals.setdefault(region, [0, 0])
    entry[0] += int(amount)
    entry[1] += 1
rows = [{"region": r, "total": v[0], "count": v[1]} for r, v in totals.items()]
rows = sorted(rows, key=lambda row: (-row["total"], row["region"]))
write("output.json", json.dumps(rows))
print("done")
""",
    "latest_versions": """
kept = {}
for record in json.loads(read("records.json")):
    best = kept.get(record["id"])
    if best is None or record["version"] > best["version"]:
        kept[record["id"]] = record
write("output.json", json.dumps(list(kept.values())))
print("done")
""",
    "log_errors": """
result = {}
for line in read("app.log").split("\\n"):
    if not line.strip():
        continue
    parts = line.split(" ", 2)
    if parts[1] != "ERROR":
        continue
    component, message = parts[2].split(": ", 1)
    if component not in result:
        result[component] = {"count": 0, "first": message}
    result[component]["count"] += 1
write("output.json", json.dumps(result))
print("done")
""",
    "tree_sizes": """
paths = sorted(json.loads(tools.call("fs.list", json.dumps({"prefix": "docs/"}))))
tree = {}
for path in paths:
    parts = path[5:].split("/")
    node = tree
    for part in parts[:-1]:
        node = node.setdefault(part, {})
    node[parts[-1]] = len(read(path))
def ordered(node):
    out = {}
    for key in sorted(node):
        value = node[key]
        out[key] = ordered(value) if isinstance(value, dict) else value
    return out
write("output.json", json.dumps(ordered(tree)))
print("done")
""",
    "validate_records": """
records = json.loads(read("records.json"))
schema = json.loads(read("schema.json"))
def kind(value):
    if value is None:
        return "null"
    if value is True or value is False:
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    return "object"
valid = []
invalid = {}
for record in records:
    problems = ["missing:" + f for f in schema["required"] if f not in record]
    for field, expected in schema["types"].items():
        if field in record:
            actual = kind(record[field])
            if not (actual == expected or (expected == "number" and actual == "integer")):
                problems.append("type:" + field)
    if problems:
        invalid[record["id"]] = sorted(problems)
    else:
        valid.append(record["id"])
write("output.json", json.dumps({"valid": valid, "invalid": invalid}))
print("done")
""",
    "tag_frequency": """
cursor = None
first = {}
while True:
    page = json.loads(tools.call("api.page", json.dumps({"cursor": cursor})))
    for item in page["items"]:
        if item["id"] not in first:
            first[item["id"]] = item["tags"]
    cursor = page["next"]
    if cursor is None:
        break
counts = {}
for tags in first.values():
    for tag in set(tags):
        counts[tag] = counts.get(tag, 0) + 1
ordered = {}
for tag, count in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
    ordered[tag] = count
write("output.json", json.dumps(ordered))
print("done")
""",
    "markdown_toc": """
out = []
used = {}
fenced = False
for line in read("README.md").split("\\n"):
    if line.startswith("```"):
        fenced = not fenced
        continue
    if fenced:
        continue
    level = len(line) - len(line.lstrip("#"))
    if 1 <= level <= 3 and line[level:level + 1] == " ":
        title = line[level:].strip()
        slug = "".join(c for c in title.lower() if c.isalnum() or c == " " or c == "-").replace(" ", "-")
        if slug in used:
            used[slug] += 1
            slug = slug + "-" + str(used[slug])
        else:
            used[slug] = 0
        out.append("  " * (level - 1) + "- [" + title + "](#" + slug + ")\\n")
write("toc.md", "".join(out))
print("done")
""",
    "interval_merge": """
rooms = {}
bookings = sorted(json.loads(read("bookings.json")), key=lambda b: (b["room"], b["start"], b["end"]))
for b in bookings:
    spans = rooms.setdefault(b["room"], [])
    if spans and b["start"] <= spans[-1][1]:
        spans[-1][1] = max(spans[-1][1], b["end"])
    else:
        spans.append([b["start"], b["end"]])
result = {}
for room in sorted(rooms):
    result[room] = rooms[room]
write("output.json", json.dumps(result))
print("done")
""",
    "json_diff": """
before = json.loads(read("before.json"))
after = json.loads(read("after.json"))
def same(a, b):
    if a is None or b is None or a is True or a is False or b is True or b is False:
        return (a is b)
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return sorted(a) == sorted(b) and all(same(a[k], b[k]) for k in a)
    return isinstance(a, str) and isinstance(b, str) and a == b
result = {
    "added": sorted(k for k in after if k not in before),
    "removed": sorted(k for k in before if k not in after),
    "changed": sorted(k for k in before if k in after and not same(before[k], after[k])),
}
write("output.json", json.dumps(result))
print("done")
""",
    "order_rollup": """
totals = {}
for order in json.loads(read("orders.json")):
    per = totals.setdefault(order["customer"], {})
    for item in order["items"]:
        per[item["sku"]] = per.get(item["sku"], 0) + item["qty"]
result = {}
for customer, per in totals.items():
    kept = {sku: qty for sku, qty in per.items() if qty != 0}
    if kept:
        result[customer] = kept
write("output.json", json.dumps(result))
print("done")
""",
    "template_render": """
template = read("template.txt")
values = json.loads(read("values.json"))
allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
out = []
missing = set()
replaced = 0
i = 0
while i < len(template):
    if template.startswith("{{", i):
        end = template.find("}}", i + 2)
        name = template[i + 2:end] if end >= 0 else ""
        if end >= 0 and name and all(c in allowed for c in name):
            if name in values:
                out.append(str(values[name]))
                replaced += 1
            else:
                out.append(template[i:end + 2])
                missing.add(name)
            i = end + 2
            continue
    out.append(template[i])
    i += 1
write("output.txt", "".join(out))
write("report.json", json.dumps({"replaced": replaced, "missing": sorted(missing)}))
print("done")
""",
}


def reference(task: str) -> str:
    return PRELUDE + REFERENCES[task]
