import json
import sys

from workflow import simulate


def main():
    try:
        data = json.loads(sys.stdin.read())
        if (
            not isinstance(data, dict)
            or "tasks" not in data
            or set(data) - {"tasks", "workers", "retries"}
        ):
            raise ValueError("request")
        result = simulate(data["tasks"], data.get("workers", 2), data.get("retries", 1))
        print(json.dumps(result))
        return 0
    except (ValueError, TypeError):
        print(json.dumps({"error": "ValueError"}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
