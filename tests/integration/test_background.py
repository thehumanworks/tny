#!/usr/bin/env python3
"""End-to-end: `tny ask -B` background turns (docs/adr/0031).

Runs the whole background lifecycle against the mock OpenAI provider:
launch prints a 16-hex session id fast, session.json is status:"running"
before the id prints, the detached child streams into task.log and
finalizes status/exit_code/result; the stored result equals what the
foreground `ask --json` prints for the same fixture. Hanging turns (the
mock's MOCK_SLOW_MS knob) exercise the writer lock: bare --resume refuses
with "still running", `tny session stop` group-SIGTERMs to
status:"interrupted" and frees the lock, a SIGKILLed group reads as
"stale" (flock self-release, decision 5), and `--resume --steer`
interrupt-and-redirects onto a working mock. Detach hygiene: the child
survives its launcher's process group being torn down.

With TNY_TEST_EXPECT_WASM=1 (the wasm CI job) the suite runs ONLY the
decision-10 check: the browser build has no fork(2), so `-B` must be a
clean exit-1 error before any backend work.
"""
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = sys.argv[1] if len(sys.argv) > 1 else \
    os.environ.get("TNY", os.path.join(ROOT, "build", "tny"))
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")

HEX16 = "0123456789abcdef"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_mock(**extra):
    """One mock per hanging scenario: the mock's HTTPServer is
    single-threaded, so a handler sleeping in MOCK_SLOW_MS blocks every
    later request to the same instance."""
    port = free_port()
    m = subprocess.Popen(
        [sys.executable, MOCK, str(port)],
        env=dict(os.environ, MOCK_EXPECT_WIRE="responses", **extra),
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    line = m.stdout.readline().decode()
    assert "ready" in line, f"mock did not start: {line!r}"
    return m, port


def poll(pred, timeout_s, what):
    """Bounded condition wait — never a bare sleep (flake policy)."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        v = pred()
        if v:
            return v
        time.sleep(0.05)
    raise AssertionError(f"timed out after {timeout_s}s waiting for {what}")


def pid_gone(pid):
    try:
        os.kill(pid, 0)
        return False
    except ProcessLookupError:
        return True
    except PermissionError:
        return False


def is_hex16(s):
    return len(s) == 16 and all(c in HEX16 for c in s)


def wasm_check():
    """Decision 10: no fork in the browser build — clean error, exit 1.
    Wired into the wasm CI job with TNY=build/wasm/tny (CI-verified;
    emsdk is not available locally)."""
    r = subprocess.run([TNY, "ask", "-B", "x"], capture_output=True, timeout=60)
    assert r.returncode == 1, f"exit {r.returncode}: {r.stderr.decode()}"
    assert b"--background is not available in the browser build" in r.stderr, \
        r.stderr
    print("test_background: wasm clean-error assertion passed")


class Ctx:
    def __init__(self, home):
        self.home = home
        self.ws = os.path.join(home, "ws")
        os.makedirs(self.ws)
        for name in ("a.txt", "b.txt", "c.txt"):
            open(os.path.join(self.ws, name), "w").write("x\n")

    def env(self, port, **kw):
        return dict(os.environ, HOME=self.home,
                    OPENAI_BASE_URL=f"http://127.0.0.1:{port}/v1",
                    OPENAI_API_KEY="test-key-not-real", **kw)

    def sdir(self, sid):
        import glob
        hits = glob.glob(os.path.join(self.home, ".tny", "sessions", "*", sid))
        assert len(hits) == 1, f"session dir for {sid}: {hits}"
        return hits[0]

    def doc(self, sid):
        return json.load(open(os.path.join(self.sdir(sid), "session.json")))

    def launch_bg(self, port, prompt, wait_pid=True):
        """ask -B, return (sid, sdir, child_pid)."""
        r = subprocess.run([TNY, "--cwd", self.ws, "ask", "-B", prompt],
                           env=self.env(port), capture_output=True, timeout=15)
        assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
        sid = r.stdout.decode().strip()
        assert is_hex16(sid), f"not a session id: {sid!r}"
        sdir = self.sdir(sid)
        pid = None
        if wait_pid:
            pidfile = os.path.join(sdir, "pid")
            poll(lambda: os.path.exists(pidfile), 10, f"{sid} pid file")
            pid = int(open(pidfile).read())
        return sid, sdir, pid


def main():
    fast, fport = start_mock()
    ctx = None
    hang_mocks = []
    try:
        with tempfile.TemporaryDirectory() as home:
            ctx = Ctx(home)
            env = ctx.env(fport)

            # ---- happy path: fast launch, running-before-print, done ----
            t0 = time.time()
            r = subprocess.run(
                [TNY, "--cwd", ctx.ws, "ask", "-B", "list files in ."],
                env=env, capture_output=True, timeout=15)
            wall = time.time() - t0
            assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
            assert wall < 2.5, f"-B launch took {wall:.2f}s (must detach fast)"
            sid = r.stdout.decode().strip()
            assert is_hex16(sid), f"not a 16-hex id: {sid!r}"
            # saved with status running BEFORE the id printed: readable now
            d = ctx.doc(sid)
            assert d.get("status") in ("running", "done"), d.get("status")
            d = poll(lambda: (lambda x: x if x.get("status") != "running"
                              else None)(ctx.doc(sid)), 20, "happy-path done")
            assert d["status"] == "done", d["status"]
            assert d["exit_code"] == 0, d
            res = d["result"]
            assert "MOCK-OK" in res["output"], res
            assert res["exit_code"] == 0, res
            assert res["ephemeral"] is False, res
            # the openai engine's tool trace must be stored, not the empty
            # host-tools fallback branch
            assert [t["name"] for t in res["tool_calls"]] == \
                ["list_files", "glob_files"], res
            assert all(t["status"] == "success" for t in res["tool_calls"]), res
            sdir = ctx.sdir(sid)
            assert os.path.getsize(os.path.join(sdir, "task.log")) > 0, \
                "task.log is empty"
            child = int(open(os.path.join(sdir, "pid")).read())
            poll(lambda: pid_gone(child), 10, "happy-path child exit")
            # plain `tny session <id>` must be readable: transcript text and
            # the stored result, not just counts (04b observability)
            rv0 = subprocess.run([TNY, "--cwd", ctx.ws, "session", sid],
                                 env=env, capture_output=True, timeout=15)
            assert rv0.returncode == 0, rv0.stderr.decode()
            view = rv0.stdout.decode()
            assert "user:" in view and "list files in ." in view, view
            assert "result:" in view and "MOCK-OK" in view, view
            assert "⏺ list_files" in view, view
            print("ok: happy path")

            # ---- result parity: stored result == foreground --json ----
            rf = subprocess.run(
                [TNY, "--cwd", ctx.ws, "ask", "--json", "list files in ."],
                env=env, capture_output=True, timeout=30)
            assert rf.returncode == 0, rf.stderr.decode()
            fg = json.loads(rf.stdout)
            norm = lambda o: {k: v for k, v in o.items() if k != "session_id"}
            assert norm(res) == norm(fg), \
                f"stored result != foreground json:\n{norm(res)}\n{norm(fg)}"
            assert is_hex16(res["session_id"]) and res["session_id"] == sid, res
            print("ok: result parity")

            # ---- ephemeral foreground --json: no session behind the fields ----
            rp = subprocess.run(
                [TNY, "--cwd", ctx.ws, "ask", "--ephemeral", "--json",
                 "list files in ."],
                env=env, capture_output=True, timeout=30)
            assert rp.returncode == 0, rp.stderr.decode()
            ep = json.loads(rp.stdout)
            assert ep["ephemeral"] is True, ep
            assert ep["session_id"] == "", ep
            print("ok: ephemeral fields in result json")

            # ---- --json launch shape ----
            rj = subprocess.run(
                [TNY, "--cwd", ctx.ws, "ask", "-B", "--json", "list files in ."],
                env=env, capture_output=True, timeout=15)
            assert rj.returncode == 0, rj.stderr.decode()
            oj = json.loads(rj.stdout)
            assert oj["kind"] == "ask_background", oj
            assert is_hex16(oj["session_id"]), oj
            assert isinstance(oj["pid"], int) and oj["pid"] > 0, oj
            jd = poll(lambda: (lambda x: x if x.get("status") != "running"
                               else None)(ctx.doc(oj["session_id"])),
                      20, "--json launch done")
            assert jd["status"] == "done", jd
            print("ok: --json launch shape")

            # ---- lock contention + stop (one hanging run) ----
            slow, sp = start_mock(MOCK_SLOW_MS="600000")
            hang_mocks.append(slow)
            hsid, hdir, hpid = ctx.launch_bg(sp, "hang for the lock test")
            rc = subprocess.run(
                [TNY, "--cwd", ctx.ws, "ask", "--resume", hsid, "x"],
                env=env, capture_output=True, timeout=15)
            assert rc.returncode == 1, \
                f"exit {rc.returncode}: {rc.stderr.decode()}"
            want = f"tny: session {hsid} is still running (pid {hpid})"
            assert want in rc.stderr.decode(), rc.stderr.decode()
            # the refusal must be actionable: watch / stop / steer hints
            assert f"tny session stop {hsid}" in rc.stderr.decode(), \
                rc.stderr.decode()
            assert "--steer" in rc.stderr.decode(), rc.stderr.decode()
            print("ok: lock contention refusal")

            # ---- live readability: nothing streamed yet says so; a
            # checkpointed partial is printed as text, not a byte count ----
            rl0 = subprocess.run([TNY, "--cwd", ctx.ws, "session", hsid],
                                 env=env, capture_output=True, timeout=15)
            assert rl0.returncode == 0, rl0.stderr.decode()
            assert "no output yet" in rl0.stdout.decode(), rl0.stdout.decode()
            with open(os.path.join(hdir, "recovery.json"), "w") as f:
                json.dump({"partial": "PARTIAL-SO-FAR"}, f)
            rl1 = subprocess.run([TNY, "--cwd", ctx.ws, "session", hsid],
                                 env=env, capture_output=True, timeout=15)
            assert rl1.returncode == 0, rl1.stderr.decode()
            lview = rl1.stdout.decode()
            assert "partial output (live" in lview, lview
            assert "PARTIAL-SO-FAR" in lview, lview
            print("ok: live partial readable")

            rs = subprocess.run(
                [TNY, "--cwd", ctx.ws, "session", "stop", hsid],
                env=ctx.env(fport, TNY_STOP_TIMEOUT_MS="3000"),
                capture_output=True, timeout=20)
            assert rs.returncode == 0, \
                f"exit {rs.returncode}: {rs.stderr.decode()}"
            assert "interrupted" in rs.stdout.decode(), rs.stdout.decode()
            sd = ctx.doc(hsid)
            assert sd["status"] == "interrupted", sd["status"]
            # the child self-finalized after the group SIGTERM (130); 137
            # would mean stop SIGKILLed and wrote the status itself
            assert sd["exit_code"] == 130, sd
            poll(lambda: pid_gone(hpid), 10, "stopped child exit")
            # lock is free: a resume passes the lock check and completes
            rr = subprocess.run(
                [TNY, "--cwd", ctx.ws, "ask", "--json", "--resume", hsid,
                 "list files in ."],
                env=env, capture_output=True, timeout=30)
            assert rr.returncode == 0, \
                f"exit {rr.returncode}: {rr.stderr.decode()}"
            assert b"MOCK-OK" in rr.stdout, rr.stdout
            print("ok: stop -> interrupted, lock free")

            # ---- stop is a clean no-op on a finished session ----
            rn = subprocess.run([TNY, "--cwd", ctx.ws, "session", "stop", sid],
                                env=env, capture_output=True, timeout=15)
            assert rn.returncode == 0, rn.stderr.decode()
            assert f"session {sid} is not running (status: done)" in \
                rn.stdout.decode(), rn.stdout.decode()
            rnj = subprocess.run(
                [TNY, "--cwd", ctx.ws, "session", "stop", sid, "--json"],
                env=env, capture_output=True, timeout=15)
            assert rnj.returncode == 0, rnj.stderr.decode()
            onj = json.loads(rnj.stdout)
            assert onj == {"kind": "session_stop", "session_id": sid,
                           "status": "done"}, onj
            # and no id at all is a usage error with the exact synopsis
            ru = subprocess.run([TNY, "--cwd", ctx.ws, "session", "stop"],
                                env=env, capture_output=True, timeout=15)
            assert ru.returncode == 1, ru.stderr.decode()
            assert b"session stop <id> [--kill]" in ru.stderr, ru.stderr
            print("ok: stop no-op / --json shape / usage guard")

            # ---- stale: SIGKILL the group, flock self-releases ----
            slow2, sp2 = start_mock(MOCK_SLOW_MS="600000")
            hang_mocks.append(slow2)
            ssid, _sdir2, spid = ctx.launch_bg(sp2, "hang for the stale test")
            os.killpg(spid, signal.SIGKILL)
            poll(lambda: pid_gone(spid), 10, "SIGKILLed child gone")
            rv = poll(lambda: (lambda r: r if "stale" in r.stdout.decode()
                               else None)(subprocess.run(
                                   [TNY, "--cwd", ctx.ws, "session", ssid],
                                   env=env, capture_output=True, timeout=15)),
                      10, "stale in session view")
            assert "running (stale" in rv.stdout.decode(), rv.stdout.decode()
            rl = subprocess.run([TNY, "--cwd", ctx.ws, "sessions", "--json"],
                                env=env, capture_output=True, timeout=15)
            assert rl.returncode == 0, rl.stderr.decode()
            sess = json.loads(rl.stdout)
            entries = sess if isinstance(sess, list) else sess["sessions"]
            by_id = {e["id"]: e for e in entries}
            assert by_id[ssid]["status"] == "stale", by_id[ssid]
            print("ok: stale detection")

            # ---- wedged holder: plain stop times out with the --kill hint;
            # --kill SIGKILLs the group and writes interrupted/137 itself.
            # A tny child always dies on SIGTERM, so the wedge is a stand-in
            # holder that flocks <dir>/lock, writes its pid, ignores SIGTERM.
            sdir3 = ctx.sdir(ssid)  # lock freed by the SIGKILL above
            holder = subprocess.Popen(
                [sys.executable, "-c", (
                    "import fcntl,os,signal,sys\n"
                    "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                    "d=sys.argv[1]\n"
                    "fd=os.open(os.path.join(d,'lock'),"
                    "os.O_CREAT|os.O_RDWR,0o600)\n"
                    "fcntl.flock(fd, fcntl.LOCK_EX)\n"
                    "open(os.path.join(d,'pid'),'w')"
                    ".write(str(os.getpid())+'\\n')\n"
                    "print('held', flush=True)\n"
                    "while True: signal.pause()\n"), sdir3],
                stdout=subprocess.PIPE, start_new_session=True)
            assert "held" in holder.stdout.readline().decode()
            wenv = ctx.env(fport, TNY_STOP_TIMEOUT_MS="300")
            rw = subprocess.run([TNY, "--cwd", ctx.ws, "session", "stop", ssid],
                                env=wenv, capture_output=True, timeout=20)
            assert rw.returncode == 2, \
                f"exit {rw.returncode}: {rw.stderr.decode()}"
            assert f"try: tny session stop {ssid} --kill" in \
                rw.stderr.decode(), rw.stderr.decode()
            assert holder.poll() is None, "plain stop must not SIGKILL"
            rk = subprocess.run(
                [TNY, "--cwd", ctx.ws, "session", "stop", ssid, "--kill"],
                env=wenv, capture_output=True, timeout=20)
            assert rk.returncode == 0, \
                f"exit {rk.returncode}: {rk.stderr.decode()}"
            assert "interrupted" in rk.stdout.decode(), rk.stdout.decode()
            assert holder.wait(timeout=10) == -signal.SIGKILL, holder.returncode
            kd = ctx.doc(ssid)
            assert kd["status"] == "interrupted", kd
            assert kd["exit_code"] == 137, kd
            print("ok: wedged holder -> exit 2 hint, --kill writes 137")

            # ---- steer: interrupt-and-redirect onto a working mock ----
            slow3, sp3 = start_mock(MOCK_SLOW_MS="600000")
            hang_mocks.append(slow3)
            tsid, tdir, tpid = ctx.launch_bg(sp3, "hang for the steer test")
            rst = subprocess.run(
                [TNY, "--cwd", ctx.ws, "ask", "--json", "--resume", tsid,
                 "--steer", "list files in ."],
                env=ctx.env(fport, TNY_STOP_TIMEOUT_MS="3000"),
                capture_output=True, timeout=30)
            assert rst.returncode == 0, \
                f"exit {rst.returncode}: {rst.stderr.decode()}"
            so = json.loads(rst.stdout)
            assert so["session_id"] == tsid, so
            assert "MOCK-OK" in so["output"], so
            poll(lambda: pid_gone(tpid), 10, "steered-away child exit")
            raw = open(os.path.join(tdir, "session.json")).read()
            assert "hang for the steer test" in raw, "old prompt lost by steer"
            assert "list files in ." in raw, "steer prompt not in transcript"
            print("ok: steer interrupt-and-redirect")

            # ---- detach hygiene: launcher pgroup teardown ----
            slowish, dp = start_mock(MOCK_SLOW_MS="1500")
            hang_mocks.append(slowish)
            launcher = subprocess.Popen(
                [TNY, "--cwd", ctx.ws, "ask", "-B", "list files in ."],
                env=ctx.env(dp), stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, start_new_session=True)
            dsid = launcher.stdout.readline().decode().strip()
            assert is_hex16(dsid), f"launcher printed {dsid!r}"
            # the pid file marks the end of the "starting" window (ADR
            # decision 4): setsid has run, the child left the launcher's
            # group. Before that, group signals can still reach it.
            dpidfile = os.path.join(ctx.sdir(dsid), "pid")
            poll(lambda: os.path.exists(dpidfile), 10, "detach pid file")
            # tear down the launcher's whole group mid-turn (terminal death)
            for sig in (signal.SIGHUP, signal.SIGTERM):
                try:
                    os.killpg(launcher.pid, sig)
                except (ProcessLookupError, PermissionError):
                    pass  # launcher already exited (macOS reports the dead
                    # group as EPERM): group empty, which is the same proof —
                    # nothing in it can reach the detached child
            launcher.wait(timeout=10)
            dd = poll(lambda: (lambda x: x if x.get("status") != "running"
                               else None)(ctx.doc(dsid)), 25,
                      "detached child finishing after pgroup kill")
            assert dd["status"] == "done", dd
            assert "MOCK-OK" in dd["result"]["output"], dd
            print("ok: detach hygiene")

            # ---- no orphans: every child pid observed above is gone ----
            dpid = int(open(os.path.join(ctx.sdir(dsid), "pid")).read())
            for p in (child, hpid, spid, tpid, dpid):
                poll(lambda: pid_gone(p), 10, f"pid {p} reaped")
            print("ok: no orphans")

            # ---- host-backend (ACP stub) parity: the stored result is the
            # answer even where the transcript holds only a resume pointer,
            # and host tool_calls ride the host_tools branch. Runs LAST
            # among the mock-provider scenarios: it persists
            # last_provider=acp in settings.json, which would win provider
            # resolution for every later implicit-provider invocation. ----
            agent = os.path.join(ROOT, "tests", "integration",
                                 "fake_acp_agent.py")
            aenv = lambda st: ctx.env(fport,
                                      FAKE_ACP_STATE=os.path.join(home, st))
            acp = [TNY, "--backend", "acp", "--agent", agent, "--cwd", ctx.ws]
            ra = subprocess.run(acp + ["ask", "--json", "hello"],
                                env=aenv("acp-fg.json"), capture_output=True,
                                timeout=30)
            assert ra.returncode == 0, ra.stderr.decode()
            afg = json.loads(ra.stdout)
            rb = subprocess.run(acp + ["ask", "-B", "hello"],
                                env=aenv("acp-bg.json"), capture_output=True,
                                timeout=30)
            assert rb.returncode == 0, rb.stderr.decode()
            asid = rb.stdout.decode().strip()
            assert is_hex16(asid), rb.stdout
            ad = poll(lambda: (lambda x: x if x.get("status") != "running"
                               else None)(ctx.doc(asid)), 20, "acp bg done")
            assert ad["status"] == "done", ad
            ares = ad["result"]
            assert norm(ares) == norm(afg), \
                f"acp stored result != foreground json:\n{norm(ares)}\n{norm(afg)}"
            assert ares["provider"] == "acp", ares
            assert ares["tool_calls"], "host tool_calls missing from result"
            print("ok: host-backend (acp) result parity")

            # ---- -B --ephemeral rejected ----
            re_ = subprocess.run(
                [TNY, "--cwd", ctx.ws, "ask", "-B", "--ephemeral", "x"],
                env=env, capture_output=True, timeout=15)
            assert re_.returncode == 1, \
                f"exit {re_.returncode}: {re_.stderr.decode()}"
            assert b"--background is incompatible with --ephemeral" in \
                re_.stderr, re_.stderr
            print("ok: -B --ephemeral rejected")

            # the API key must never leak into any output
            for blob in (r.stdout, r.stderr, rj.stdout, rc.stderr, rs.stdout,
                         rst.stdout):
                assert b"test-key-not-real" not in blob, "api key leaked"
        print("test_background: all assertions passed")
    finally:
        for m in [fast] + hang_mocks:
            m.terminate()
        for m in [fast] + hang_mocks:
            try:
                m.wait(timeout=5)
            except subprocess.TimeoutExpired:
                m.kill()


if __name__ == "__main__":
    start = time.time()
    if os.environ.get("TNY_TEST_EXPECT_WASM"):
        wasm_check()
    else:
        main()
    print(f"test_background: done in {time.time() - start:.1f}s")
