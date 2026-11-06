"""Benchmark-only host capability bridge for shell/Node/Monty experiments.

All tool calls are settled by the production embedded-Python cell and existing
C benchmark host. No provider credentials enter this process.
"""

import json
import os
import socket
import sys
from pathlib import Path


def rpc(operation, *args):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.connect(os.environ["TNY_EVAL_SOCKET"])
        stream = connection.makefile("rwb")
        stream.write((json.dumps([operation, *args]) + "\n").encode())
        stream.flush()
        result = json.loads(stream.readline())
        if "error" in result:
            raise RuntimeError(result["error"])
        return result["result"]


class Tools:
    def call(self, name, arguments):
        return rpc("call", name, arguments)

    def list(self):
        return rpc("list")

    def describe(self, name):
        return rpc("describe", name)


tools = Tools()

MONTY_PRELUDE = """
import json
class Tools:
    def call(self, name, arguments):
        return tool_call(name, arguments)
    def list(self):
        return tool_list()
    def describe(self, name):
        return tool_describe(name)
tools = Tools()
"""


def main():
    if sys.argv[1] == "monty":
        from pydantic_monty import Monty

        with Monty() as pool:
            with pool.checkout() as cell:
                cell.feed_run(
                    MONTY_PRELUDE + Path(sys.argv[2]).read_text(),
                    external_lookup={
                        "tool_call": tools.call,
                        "tool_list": tools.list,
                        "tool_describe": tools.describe,
                    },
                    print_callback=lambda stream, text: print(text, end=""),
                )
    else:
        value = rpc(sys.argv[1], *sys.argv[2:])
        if value is not None:
            sys.stdout.write(value)


if __name__ == "__main__":
    main()
