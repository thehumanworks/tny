#!/usr/bin/env python3
import json, os, pathlib, subprocess, tempfile

TNY = os.environ.get("TNY", "build/tny")

with tempfile.TemporaryDirectory() as td:
    d = pathlib.Path(td)
    log = d / "ssh.json"
    ssh = d / "ssh"
    ssh.write_text("""#!/usr/bin/env python3
import json, os, sys
path=os.environ['FAKE_SSH_LOG']
open(path,'w').write(json.dumps(sys.argv[1:]))
""")
    ssh.chmod(0o755)
    env = os.environ.copy()
    env["PATH"] = td + os.pathsep + env.get("PATH", "")
    env["FAKE_SSH_LOG"] = str(log)

    p = subprocess.run([TNY, "--ssh", "alice@example.test:2222", "ask", "say 'hello'; echo nope"],
                       env=env, text=True, capture_output=True)
    assert p.returncode == 0, p.stderr
    av = json.loads(log.read_text())
    assert av[:4] == ["-p", "2222", "--", "alice@example.test"], av
    cmd = av[4]
    assert "'tny'" in cmd and "'ask'" in cmd, cmd
    assert "'say '\\''hello'\\''; echo nope'" in cmd, cmd
    assert "--ssh" not in cmd, cmd

    p = subprocess.run([TNY, "--ssh", "host:0", "ask", "x"], env=env,
                       text=True, capture_output=True)
    assert p.returncode == 1 and "invalid SSH port" in p.stderr, (p.returncode, p.stderr)

    p = subprocess.run([TNY, "--ssh"], env=env, text=True, capture_output=True)
    assert p.returncode == 1 and "requires user@host" in p.stderr, p.stderr

print("ssh integration: ok")
