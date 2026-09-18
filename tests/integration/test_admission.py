#!/usr/bin/env python3
"""Standalone admission seam gate; no competing jobs scheduler or mocked locks."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    "tests/fixtures/admission.c",
    "src/core/admission.c",
    "src/util/admission_host.c",
    "src/util/jobs_host.c",
    "src/util/process.c",
    "src/util/process_scope.c",
    "src/util/tny_poll.c",
    "src/util/util.c",
    "third_party/yyjson/yyjson.c",
]


def main():
    with tempfile.TemporaryDirectory(prefix="tny-admission-") as directory:
        binary = Path(directory) / "admission"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11",
            "-D_GNU_SOURCE",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-deprecated-declarations",  # Same baseline WARN as Makefile.
            "-g",
            "-Isrc",
            "-Ithird_party/yyjson",
        ]
        command += shlex.split(os.environ.get("ADMISSION_CFLAGS", ""))
        objects = []
        for index, source in enumerate(SOURCES):
            obj = str(Path(directory) / f"{index}.o")
            faults = []
            if source == "src/util/jobs_host.c":
                faults = ["-Drename=tny_admission_test_rename"]
            elif source == "src/util/admission_host.c":
                faults = ["-Dfsync=tny_admission_test_fsync"]
            subprocess.run(
                command + faults + ["-c", source, "-o", obj], cwd=ROOT, check=True
            )
            objects.append(obj)
        subprocess.run(command + objects + ["-o", str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary), directory], cwd=ROOT, check=True, timeout=65)


if __name__ == "__main__":
    main()
