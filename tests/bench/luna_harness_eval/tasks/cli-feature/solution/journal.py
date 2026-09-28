import argparse
import json
from datetime import date
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
    stats = commands.add_parser("stats", help="summarize entries as JSON")
    stats.add_argument(
        "--since", metavar="YYYY-MM-DD", help="include dates on or after this date"
    )
    stats.add_argument("--tag", help="include only this tag")
    args = parser.parse_args(argv)
    path = Path(args.file)
    rows = json.loads(path.read_text()) if path.exists() else []
    if args.command == "add":
        rows.append(dict(date=args.date, tag=args.tag, minutes=args.minutes))
        path.write_text(json.dumps(rows))
    elif args.command == "list":
        print(json.dumps(rows))
    elif args.command == "stats":
        if args.since:
            try:
                date.fromisoformat(args.since)
            except ValueError as exc:
                parser.error(f"invalid --since: {exc}")
            rows = [r for r in rows if r["date"] >= args.since]
        if args.tag:
            rows = [r for r in rows if r["tag"] == args.tag]
        print(
            json.dumps(
                dict(count=len(rows), total_minutes=sum(r["minutes"] for r in rows))
            )
        )


if __name__ == "__main__":
    main()
