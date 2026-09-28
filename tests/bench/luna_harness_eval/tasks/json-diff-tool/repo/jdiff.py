import argparse
import json
from pathlib import Path


def diff_values(left, right, ignored=()):
    raise NotImplementedError


def main():
    parser = argparse.ArgumentParser(description="Inspect two JSON files")
    parser.add_argument("left")
    parser.add_argument("right")
    args = parser.parse_args()
    left = json.loads(Path(args.left).read_text())
    right = json.loads(Path(args.right).read_text())
    print(json.dumps({"equal": left == right}))


if __name__ == "__main__":
    main()
