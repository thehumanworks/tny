import argparse
import json
from pathlib import Path


def main(argv=None):
    parser = argparse.ArgumentParser(description="Daily work journal")
    parser.add_argument("--file", default="journal.json")
    commands = parser.add_subparsers(dest="command", required=True)
    add = commands.add_parser("add", help="add an entry")
    add.add_argument("date")
    add.add_argument("tag")
    add.add_argument("minutes", type=int)
    commands.add_parser("list", help="list entries")
    args = parser.parse_args(argv)
    path = Path(args.file)
    rows = json.loads(path.read_text()) if path.exists() else []
    if args.command == "add":
        rows.append(dict(date=args.date, tag=args.tag, minutes=args.minutes))
        path.write_text(json.dumps(rows))
    elif args.command == "list":
        print(json.dumps(rows))


if __name__ == "__main__":
    main()
