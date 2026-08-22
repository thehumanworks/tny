#!/usr/bin/env python3
"""WebSocket wrapper around fake_acp_agent.py for the --agent ws:// transport.

Stdlib only. Serves one RFC6455 endpoint; on each connection it spawns a
fresh fake_acp_agent.py subprocess and bridges frames <-> JSONL:
  * each client text frame becomes one line on the agent's stdin
  * each agent stdout line becomes one text frame back
  * agent stderr is forwarded to this process's stderr
The agent's environment knobs (FAKE_ACP_STATE, FAKE_ACP_DIE, …) pass
through unchanged, so the ws tests exercise the same scripted transcript
the stdio tests do.

Usage: fake_acp_agent_ws.py PORT   (0 picks a free port; prints "ready on N")
"""
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import threading

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
AGENT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_acp_agent.py")


def read_head(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return None, b""
        data += chunk
        if len(data) > 65536:
            return None, b""
    head, rest = data.split(b"\r\n\r\n", 1)
    headers = {}
    for line in head.decode("latin-1").split("\r\n")[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    return headers, rest


def handshake(conn):
    headers, rest = read_head(conn)
    if headers is None or "sec-websocket-key" not in headers:
        conn.close()
        return None
    accept = base64.b64encode(hashlib.sha1(
        (headers["sec-websocket-key"] + GUID).encode()).digest()).decode()
    conn.sendall((
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
    return rest


def send_text(conn, text):
    payload = text.encode()
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", 0x81, n)
    elif n < 65536:
        head = struct.pack("!BBH", 0x81, 126, n)
    else:
        head = struct.pack("!BBQ", 0x81, 127, n)
    conn.sendall(head + payload)


def recv_exact(conn, buf, n):
    while len(buf) < n:
        chunk = conn.recv(65536)
        if not chunk:
            return None
        buf += chunk
    return buf


def serve(conn):
    rest = handshake(conn)
    if rest is None:
        return
    # frames go out from two threads (agent stdout pump, ping-pong reply);
    # interleaved sends would corrupt the framing
    wlock = threading.Lock()
    proc = subprocess.Popen([sys.executable, AGENT],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=sys.stderr.fileno())

    def pump_out():
        for line in proc.stdout:
            line = line.decode().rstrip("\r\n")
            if not line:
                continue
            try:
                with wlock:
                    send_text(conn, line)
                print("ws-agent: frame out %dB" % len(line), file=sys.stderr,
                      flush=True)
            except OSError:
                break
        try:
            conn.shutdown(socket.SHUT_WR)
        except OSError:
            pass

    t = threading.Thread(target=pump_out, daemon=True)
    t.start()

    buf = rest
    try:
        while True:
            buf = recv_exact(conn, buf, 2)
            if buf is None:
                break
            b0, b1 = buf[0], buf[1]
            opcode = b0 & 0x0F
            masked = bool(b1 & 0x80)
            ln = b1 & 0x7F
            off = 2
            if ln == 126:
                buf = recv_exact(conn, buf, 4)
                if buf is None:
                    break
                ln = struct.unpack("!H", buf[2:4])[0]
                off = 4
            elif ln == 127:
                buf = recv_exact(conn, buf, 10)
                if buf is None:
                    break
                ln = struct.unpack("!Q", buf[2:10])[0]
                off = 10
            mask = b""
            if masked:
                buf = recv_exact(conn, buf, off + 4)
                if buf is None:
                    break
                mask = buf[off:off + 4]
                off += 4
            buf = recv_exact(conn, buf, off + ln)
            if buf is None:
                break
            payload = buf[off:off + ln]
            buf = buf[off + ln:]
            if masked:
                payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
            if opcode == 0x8:  # close
                break
            if opcode == 0x9:  # ping -> pong
                with wlock:
                    conn.sendall(struct.pack("!BB", 0x8A, len(payload)) + payload)
                continue
            if opcode != 0x1:
                if opcode == 0x0:
                    print("ws-agent: unexpected continuation frame (client "
                          "fragmented a message)", file=sys.stderr, flush=True)
                continue
            # sanity: one JSON object per frame
            try:
                json.loads(payload.decode())
            except ValueError:
                print("ws-agent: non-JSON frame", file=sys.stderr, flush=True)
            print("ws-agent: frame in %dB" % ln, file=sys.stderr, flush=True)
            proc.stdin.write(payload + b"\n")
            proc.stdin.flush()
    finally:
        try:
            proc.stdin.close()
        except OSError:
            pass
        proc.wait(timeout=10)
        conn.close()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)
    print("ready on %d" % srv.getsockname()[1], flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=serve, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
