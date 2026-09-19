#!/usr/bin/env python3
"""Target default-learning guards using isolated source copies; never edit the tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = """#include "greatest.h"
GREATEST_MAIN_DEFS();
SUITE_EXTERN(learning_suite);
SUITE_EXTERN(learning_process_suite);
int main(int argc,char **argv) {
 GREATEST_MAIN_BEGIN(); RUN_SUITE(learning_suite); RUN_SUITE(learning_process_suite);
 GREATEST_MAIN_END();
}
"""
MUTANTS = {
    "skip-final-flush": (
        "src/core/learning.c",
        "tny_learning_store_flush(&learning->store, learning->rules);",
        "(void)learning;",
    ),
    "discard-pending-at-next-turn": (
        "src/core/learning.c",
        "strcmp(learning->store.root, tny_dir) == 0",
        "false",
    ),
    "promote-one-observation": (
        "src/core/learning.c",
        "rule->successes < 2",
        "rule->successes < 1",
    ),
    "ignore-negative-feedback": (
        "src/core/learning.c",
        "rule->successes <= 2 * rule->failures",
        "false",
    ),
    "ignore-disabled": (
        "src/core/learning.c",
        "if (!learning || !learning->enabled) return;",
        "if (!learning) return;",
    ),
    "ignore-target": (
        "src/core/learning.c",
        "scope && scope == learning->pending_scope && learning->diagnostic >= 0",
        "scope && learning->diagnostic >= 0",
    ),
    "retain-expired-episode": (
        "src/core/learning.c",
        "++learning->intervening > 8",
        "++learning->intervening > 80",
    ),
    "overwrite-instead-of-merge": (
        "src/util/learning_store.c",
        "if (!load(dir, name, merged)) goto done;",
        "if (false) goto done;",
    ),
    "accept-unknown-schema-fields": (
        "src/util/learning_store.c",
        "yyjson_obj_size(root) != 2",
        "yyjson_obj_size(root) < 2",
    ),
    "accept-public-directory": (
        "src/util/learning_store.c",
        "dir >= 0 && !private_fd(dir, true)",
        "dir >= 0 && false",
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    source_paths = [
        "src/core/learning.c",
        "src/util/learning_store.c",
        "src/util/util.c",
        "third_party/yyjson/yyjson.c",
        "tests/test_learning.c",
    ]
    inputs = {
        p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in source_paths
    }
    rows = []
    with tempfile.TemporaryDirectory(prefix="tny-learning-mutations-") as temp:
        root = Path(temp)
        driver = root / "main.c"
        driver.write_text(MAIN)
        for name in ["baseline", *MUTANTS]:
            sources = list(source_paths)
            if name != "baseline":
                file, old, new = MUTANTS[name]
                source = (ROOT / file).read_text()
                if source.count(old) != 1:
                    raise ValueError(f"non-unique mutation anchor: {name}")
                changed = root / (name + ".c")
                changed.write_text(source.replace(old, new, 1))
                sources[sources.index(file)] = str(changed)
            binary = root / name
            compile_result = subprocess.run(
                [
                    *shlex.split(os.environ.get("CC", "cc")),
                    "-std=c11",
                    "-D_DARWIN_C_SOURCE",
                    "-D_DEFAULT_SOURCE",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-deprecated-declarations",
                    "-Isrc",
                    "-Ithird_party/yyjson",
                    "-Ithird_party/greatest",
                    str(driver),
                    *sources,
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=60,
            )
            if compile_result.returncode:
                rows.append(
                    {
                        "name": name,
                        "passed": False,
                        "classification": "invalid build",
                        "stderr": compile_result.stderr,
                    }
                )
                break
            result = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=15
            )
            expected = 0 if name == "baseline" else 1
            rows.append(
                {
                    "name": name,
                    "exit_code": result.returncode,
                    "passed": result.returncode == expected,
                    "classification": "pristine"
                    if name == "baseline"
                    else "killed"
                    if result.returncode == 1
                    else "survived",
                    "stdout": result.stdout,
                    "stderr": result.stderr,
                }
            )
            if name == "baseline" and result.returncode:
                break
    report = {
        "inputs_sha256": inputs,
        "results": rows,
        "passed": len(rows) == len(MUTANTS) + 1 and all(row["passed"] for row in rows),
    }
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "passed": report["passed"],
                "results": [
                    {k: row[k] for k in ("name", "classification", "passed")}
                    for row in rows
                ],
            },
            indent=2,
        )
    )
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
