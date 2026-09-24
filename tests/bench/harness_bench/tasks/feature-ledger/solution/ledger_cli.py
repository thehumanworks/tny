import json
import sys

from ledger import Ledger


def main():
    if len(sys.argv) != 2:
        return 2
    failed = False
    with Ledger(sys.argv[1]) as ledger:
        for line in sys.stdin:
            try:
                request = json.loads(line)
                if not isinstance(request, dict) or set(request) != {
                    "event_id",
                    "sku",
                    "delta",
                }:
                    raise ValueError("shape")
                value = ledger.apply(
                    request["event_id"], request["sku"], request["delta"]
                )
                print(json.dumps({"stock": value}))
            except (ValueError, TypeError):
                print(json.dumps({"error": "ValueError"}))
                failed = True
    return 2 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
