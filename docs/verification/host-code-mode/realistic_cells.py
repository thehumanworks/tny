#!/usr/bin/env python3
"""Realistic host work in production code cells (ADR 0180 evidence).

Each scenario runs one fresh `tny --code-cell` child through the private cell
protocol, exactly as the execution server starts it (fd 3 socket, stdout and
stderr on a drained pipe, working directory in the start frame). Everything
happens in a private temporary directory and on loopback; no credentials,
provider or external network are used. Prints one JSON report.

    python3 docs/verification/host-code-mode/realistic_cells.py build/tny
"""

from __future__ import annotations

import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def frame(data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + data


def exact(sock: socket.socket, size: int) -> bytes | None:
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def run_cell(tny: str, code: str, cwd: str, timeout: float = 120) -> dict:
    parent, child = socket.socketpair()
    read, write = os.pipe()
    started = time.monotonic()
    process = subprocess.Popen(
        [
            sys.executable,
            "-c",
            "import os,sys; os.dup2(int(sys.argv[1]),3); os.set_inheritable(3,True); "
            "os.execv(sys.argv[2],[sys.argv[2],'--code-cell'])",
            str(child.fileno()),
            tny,
        ],
        pass_fds=(child.fileno(),),
        stdin=subprocess.DEVNULL,
        stdout=write,
        stderr=write,
    )
    child.close()
    os.close(write)
    body = b"S%d:%d:" % (2, len(cwd.encode())) + b"[]" + cwd.encode() + code.encode()
    parent.sendall(frame(body))
    parent.settimeout(timeout)
    output = bytearray()
    done = threading.Event()

    def drain():
        while chunk := os.read(read, 65536):
            output.extend(chunk)
        done.set()

    threading.Thread(target=drain, daemon=True).start()
    header = exact(parent, 4)
    final = exact(parent, struct.unpack(">I", header)[0]) if header else None
    process.wait(timeout=timeout)
    done.wait(5)
    return {
        "exit": process.returncode,
        "summary": (final or b"<none>")[1:].decode(errors="replace"),
        "seconds": round(time.monotonic() - started, 3),
        "output": output.decode(errors="replace"),
    }


class Files(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        body = json.dumps(
            {"items": [{"id": i, "name": f"item-{i}"} for i in range(500)]}
        )
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body.encode())


SCENARIOS = {
    "c_build_and_test": r"""
import pathlib, shutil, subprocess
if not shutil.which("cc"):
    print("SKIP no cc"); raise SystemExit
pathlib.Path("add.c").write_text('#include <stdio.h>\nint main(void){printf("%d\\n", 40+2);return 0;}\n')
subprocess.run(["cc", "-O2", "-o", "add", "add.c"], check=True)
print("built", subprocess.run(["./add"], capture_output=True, text=True).stdout.strip())
""",
    "git_repository": r"""
import shutil, subprocess
if not shutil.which("git"):
    print("SKIP no git"); raise SystemExit
env = {"GIT_AUTHOR_NAME": "cell", "GIT_AUTHOR_EMAIL": "cell@example.invalid",
       "GIT_COMMITTER_NAME": "cell", "GIT_COMMITTER_EMAIL": "cell@example.invalid"}
import os
env = {**os.environ, **env}
subprocess.run(["git", "init", "-q", "repo"], check=True)
open("repo/a.txt", "w").write("one\n")
subprocess.run(["git", "-C", "repo", "add", "a.txt"], check=True, env=env)
subprocess.run(["git", "-C", "repo", "commit", "-qm", "first"], check=True, env=env)
print(subprocess.run(["git", "-C", "repo", "log", "--oneline"], capture_output=True, text=True).stdout.split()[1:])
""",
    "http_json_to_sqlite": r"""
import json, sqlite3, urllib.request
with urllib.request.urlopen(URL, timeout=10) as reply:
    items = json.load(reply)["items"]
db = sqlite3.connect("items.db")
db.execute("create table items(id integer primary key, name text)")
db.executemany("insert into items values (?, ?)", [(i["id"], i["name"]) for i in items])
db.commit()
print("rows", db.execute("select count(*), max(id) from items").fetchone())
""",
    "archives_and_hashes": r"""
import hashlib, io, lzma, os, pathlib, tarfile, zipfile
pathlib.Path("tree/sub").mkdir(parents=True)
for n in range(20):
    pathlib.Path(f"tree/sub/f{n}.txt").write_text("data %d\n" % n * 100)
with tarfile.open("tree.tar.xz", "w:xz") as t:
    t.add("tree")
with tarfile.open("tree.tar.xz") as t:
    names = t.getnames()
with zipfile.ZipFile("tree.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for p in pathlib.Path("tree").rglob("*.txt"):
        z.write(p)
digest = hashlib.sha256(pathlib.Path("tree.tar.xz").read_bytes()).hexdigest()
print(len(names), zipfile.ZipFile("tree.zip").testzip(), len(digest))
""",
    "concurrency": r"""
import asyncio, concurrent.futures, multiprocessing, subprocess
async def main():
    procs = [await asyncio.create_subprocess_exec("sh", "-c", f"echo {i}", stdout=asyncio.subprocess.PIPE) for i in range(4)]
    return sorted([int((await p.communicate())[0]) for p in procs])
print("asyncio", asyncio.run(main()))
with concurrent.futures.ThreadPoolExecutor(4) as pool:
    print("threads", sum(pool.map(lambda x: x * x, range(10))))
def square(x):
    return x * x
with multiprocessing.get_context("fork").Pool(2) as pool:
    print("fork pool", pool.map(square, [1, 2, 3]))
""",
    "verbose_subprocess_output": r"""
import subprocess
subprocess.run(["sh", "-c", "i=0; while [ $i -lt 20000 ]; do echo line $i; i=$((i+1)); done"])
print("after the noisy command")
""",
}


def main() -> int:
    tny = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/tny").resolve())
    server = ThreadingHTTPServer(("127.0.0.1", 0), Files)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_port}/items"
    report = {}
    try:
        for name, code in SCENARIOS.items():
            with tempfile.TemporaryDirectory(prefix=f"tny-cell-{name}-") as workspace:
                result = run_cell(tny, f"URL = {url!r}\n" + code, workspace)
                output = result.pop("output")
                result["output_bytes"] = len(output.encode())
                result["output_tail"] = output[-240:]
                report[name] = result
    finally:
        server.shutdown()
    print(json.dumps(report, indent=2))
    return 0 if all(r["exit"] == 0 and not r["summary"] for r in report.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
