import argparse
import json
from pathlib import Path


def diff_values(left, right, ignored=()):
    changes = []
    ignored = tuple(ignored)

    def escaped(value):
        return str(value).replace("~", "~0").replace("/", "~1")

    def skip(path):
        return any(path == item or path.startswith(item + "/") for item in ignored)

    def walk(a, b, path):
        if skip(path):
            return
        if type(a) is not type(b):
            changes.append({"path": path, "kind": "changed", "before": a, "after": b})
        elif isinstance(a, dict):
            for key in sorted(a.keys() | b.keys()):
                child = path + "/" + escaped(key)
                if skip(child):
                    continue
                if key not in a:
                    changes.append({"path": child, "kind": "added", "after": b[key]})
                elif key not in b:
                    changes.append({"path": child, "kind": "removed", "before": a[key]})
                else:
                    walk(a[key], b[key], child)
        elif isinstance(a, list):
            for index in range(max(len(a), len(b))):
                child = path + "/" + str(index)
                if skip(child):
                    continue
                if index >= len(a):
                    changes.append({"path": child, "kind": "added", "after": b[index]})
                elif index >= len(b):
                    changes.append(
                        {"path": child, "kind": "removed", "before": a[index]}
                    )
                else:
                    walk(a[index], b[index], child)
        elif a != b:
            changes.append({"path": path, "kind": "changed", "before": a, "after": b})

    walk(left, right, "")
    return sorted(changes, key=lambda item: (item["path"], item["kind"]))


def main(argv=None):
    parser = argparse.ArgumentParser(description="Diff two JSON files")
    parser.add_argument("left")
    parser.add_argument("right")
    parser.add_argument(
        "--ignore",
        action="append",
        default=[],
        metavar="POINTER",
        help="skip a path and descendants",
    )
    parser.add_argument("--summary", action="store_true", help="print change counts")
    args = parser.parse_args(argv)
    left = json.loads(Path(args.left).read_text())
    right = json.loads(Path(args.right).read_text())
    changes = diff_values(left, right, args.ignore)
    if args.summary:
        print(
            json.dumps(
                {
                    kind: sum(c["kind"] == kind for c in changes)
                    for kind in ("added", "removed", "changed")
                }
            )
        )
    else:
        print(json.dumps(changes))


if __name__ == "__main__":
    main()
