"""Native host-capability acceptance through real provider wires and local fixtures.

All files, environment markers, HTTP traffic and certificates are synthetic.
No provider account or public-network request is used.
"""

import json
import ssl
import subprocess
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/integration"))
from test_execution_code_mode import WASM, ExecutionCodeMode


class Endpoint(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.paths.append(self.path)
        body = b"host-network-ok"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


@unittest.skipIf(
    WASM, "host operations require native execution; wasm refusal is tested separately"
)
class HostAcceptance(ExecutionCodeMode):
    def test_direct_host_capabilities(self):
        endpoint = ThreadingHTTPServer(("127.0.0.1", 0), Endpoint)
        endpoint.paths = []
        worker = threading.Thread(target=endpoint.serve_forever, daemon=True)
        worker.start()
        try:
            for wire in ("responses", "chat"):
                marker = self.workspace / ("host-" + wire + ".txt")
                local_module = self.workspace / "tny_host_probe.py"
                local_module.write_text("VALUE = 73\n")
                code = f"""import os, pathlib, subprocess, socket, urllib.request, threading
import tny_host_probe
assert tny_host_probe.VALUE == 73
assert os.getcwd() == {str(self.workspace)!r}, os.getcwd()
assert os.environ['TNY_HOST_TEST_SENTINEL'] == 'synthetic-only'
assert os.environ['HOME'] == {str(self.home)!r}
p = pathlib.Path({str(marker)!r})
with open(p, 'w', encoding='utf-8') as f:
    f.write('direct-host-α')
assert p.read_text(encoding='utf-8') == 'direct-host-α'
child = subprocess.run(['/bin/sh', '-c', 'printf subprocess-ok'], capture_output=True, text=True, check=True)
assert child.stdout == 'subprocess-ok'
values=[]
t = threading.Thread(target=lambda: values.append(42))
t.start(); t.join()
assert values == [42]
url={f"http://127.0.0.1:{endpoint.server_port}/host-" + wire!r}
with urllib.request.urlopen(url, timeout=2) as r:
    body=r.read().decode('utf-8')
assert body == 'host-network-ok'
assert eval('6 * 7') == 42
exec('dynamic_value = 19')
assert dynamic_value == 19
print(json.dumps({{'host':'ok','cwd':os.getcwd(),'child':child.stdout,'network':body,'value':values[0]}}))
"""
                output = self.run_code(
                    code, wire=wire, env={"TNY_HOST_TEST_SENTINEL": "synthetic-only"}
                )
                self.assertFalse(output.startswith("error:"), output)
                value = json.loads(output)
                self.assertEqual(value["host"], "ok")
                self.assertEqual(marker.read_text(), "direct-host-α")
                self.assert_schema(wire)
            self.assertEqual(endpoint.paths, ["/host-responses", "/host-chat"])
        finally:
            endpoint.shutdown()
            worker.join(3)
            endpoint.server_close()

    def test_verified_https(self):
        cert = self.home / "test-cert.pem"
        key = self.home / "test-key.pem"
        subprocess.run(
            [
                "openssl",
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-nodes",
                "-keyout",
                str(key),
                "-out",
                str(cert),
                "-days",
                "1",
                "-subj",
                "/CN=localhost",
                "-addext",
                "subjectAltName=DNS:localhost,IP:127.0.0.1",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
        )
        endpoint = ThreadingHTTPServer(("127.0.0.1", 0), Endpoint)
        endpoint.paths = []
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        endpoint.socket = context.wrap_socket(endpoint.socket, server_side=True)
        worker = threading.Thread(target=endpoint.serve_forever, daemon=True)
        worker.start()
        try:
            code = (
                "import ssl, urllib.request\n"
                + f"ctx=ssl.create_default_context(cafile={str(cert)!r})\n"
                + "assert ctx.verify_mode == ssl.CERT_REQUIRED and ctx.check_hostname\n"
                + f"with urllib.request.urlopen('https://localhost:{endpoint.server_port}/verified', context=ctx, timeout=3) as response:\n"
                + "    print(response.read().decode('utf-8'))\n"
            )
            output = self.run_code(code)
            self.assertEqual(output, "host-network-ok\n")
            self.assertEqual(endpoint.paths, ["/verified"])
        finally:
            endpoint.shutdown()
            worker.join(3)
            endpoint.server_close()


if __name__ == "__main__":
    suite = unittest.TestSuite(
        [
            HostAcceptance("test_direct_host_capabilities"),
            HostAcceptance("test_verified_https"),
        ]
    )
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
