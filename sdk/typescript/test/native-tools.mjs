import assert from "node:assert/strict";
import {
  copyFileSync, cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { spawn } from "node:child_process";

const here = dirname(fileURLToPath(import.meta.url));
const delay = (ms) => new Promise((done) => setTimeout(done, ms));

async function childCase() {
  const { Runtime } = await import(pathToFileURL(join(process.env.TNY_TEST_STAGE, "dist/index.mjs")));
  const workspace = process.env.TNY_TEST_WORKSPACE;
  const kind = process.env.TNY_TEST_CASE;
  assert.equal(process.env.PATH, join(workspace, "empty-path"));
  assert.ok(!existsSync(join(process.env.PATH, "tny")));
  const options = {
    workspace, stateDir: join(workspace, "state"), persistence: true,
    baseUrl: process.env.TNY_TEST_BASE_URL, apiKey: "synthetic-not-a-key",
    wireApi: process.env.TNY_TEST_WIRE, permissionMode: kind === "deny" ? "ask" : "yolo",
    ...(kind === "disabled" ? { codeExecution: false } : {}),
  };
  let runtime = await Runtime.create(options);
  let session;
  try {
    assert.equal(runtime.capabilities.codeExecution, kind !== "disabled");
    session = await runtime.createSession();
    const id = session.id;
    const events = [];
    const abort = new AbortController();
    let cancelTask;
    if (kind === "cancel") cancelTask = (async () => {
      const deadline = Date.now() + 15_000;
      while (!existsSync(join(workspace, "started"))) {
        if (Date.now() >= deadline) throw new Error("Python cell never started");
        await delay(10);
      }
      abort.abort();
    })();
    try {
      for await (const event of session.run("Run the synthetic native SDK fixture", { signal: abort.signal }))
        events.push(event);
    } finally {
      if (cancelTask) await cancelTask;
    }
    assert.equal(events.at(-1)?.type, "turn_end");
    const ends = events.filter((event) => event.type === "tool_end" && event.toolName === "run_code");
    if (kind === "cancel") {
      assert.equal(events.at(-1)?.stopReason, "interrupted");
      await delay(2300);
      assert.ok(!existsSync(join(workspace, "late")), "cancelled worker must not finish its delayed effect");
    } else {
      assert.equal(events.at(-1)?.stopReason, kind === "deny" ? "denied" : "done");
      assert.equal(ends.length, 1);
      if (kind !== "deny") assert.ok(events.some((event) => event.type === "text_delta"));
      if (kind === "disabled") {
        assert.equal(ends[0].toolOk, false);
        assert.match(ends[0].toolDetail, /execution server unavailable/);
        assert.ok(!existsSync(join(workspace, "written")));
      } else if (kind === "deny") {
        assert.ok(!existsSync(join(workspace, "written")));
        assert.equal(events.filter((event) => event.type === "permission_request").length, 0);
        assert.match(ends[0].toolDetail, /denied|approval|permission/i);
      } else {
        assert.equal(ends[0].toolOk, true, ends[0].toolDetail);
        assert.equal(readFileSync(join(workspace, "written"), "utf8"), "native write");
        assert.equal(readFileSync(join(workspace, "shell"), "utf8"), "native shell");
        assert.equal(readFileSync(join(workspace, "count"), "utf8"), "1");
        assert.match(ends[0].toolDetail, /native write/);
        assert.match(ends[0].toolDetail, /restricted surface checked/);
        await session.close(); session = undefined;
        await runtime.close();
        runtime = await Runtime.create(options);
        session = await runtime.openSession(id);
        await session.ask("Continue without replaying completed work");
        assert.equal(readFileSync(join(workspace, "count"), "utf8"), "1", "resume must not replay a completed effect");
      }
    }
    console.log(JSON.stringify({ kind, wire: options.wireApi, events: events.length, cli: "absent" }));
  } finally {
    if (session) await session.close();
    await runtime.close();
  }
}

async function withMock(mockScript, env, run) {
  const mock = spawn(process.env.PYTHON || "python3", [mockScript, "0"], {
    env: { ...process.env, ...env }, stdio: ["ignore", "pipe", "pipe"],
  });
  let diagnostics = "";
  mock.stderr.on("data", (data) => { diagnostics += data; });
  const exited = new Promise((done) => mock.once("exit", done));
  try {
    const port = await new Promise((ready, reject) => {
      let output = "";
      const timer = setTimeout(() => reject(new Error(`mock readiness timeout: ${diagnostics}`)), 15_000);
      mock.once("error", (error) => { clearTimeout(timer); reject(error); });
      mock.once("exit", (code) => { clearTimeout(timer); reject(new Error(`mock exited ${code}: ${diagnostics}`)); });
      mock.stdout.on("data", (data) => {
        output += data;
        const match = /^ready on (\d+)\r?$/m.exec(output);
        if (match) { clearTimeout(timer); ready(Number(match[1])); }
      });
    });
    await run(`http://127.0.0.1:${port}/v1`);
  } finally {
    if (mock.exitCode === null && mock.signalCode === null) mock.kill("SIGTERM");
    await exited;
  }
}

async function main() {
  const source = process.env.TNY_SDK_PACKAGE_ROOT
    ? resolve(process.env.TNY_SDK_PACKAGE_ROOT) : resolve(here, "..");
  const { resolveNativeAddon } = await import(pathToFileURL(join(source, "scripts/native-loader.mjs")));
  const native = resolveNativeAddon({ packageRoot: source });
  assert.ok(native.executorPath, "build the native executor before this acceptance test");
  const root = mkdtempSync(join(tmpdir(), "tny-native-sdk-"));
  const stage = join(root, "package");
  const build = join(stage, "build/Release");
  mkdirSync(build, { recursive: true });
  try {
    for (const path of ["dist", "scripts", "package.json"])
      cpSync(join(source, path), join(stage, path), { recursive: true });
    copyFileSync(native.addonPath, join(build, "tny.node"));
    copyFileSync(native.libraryPath, join(build, process.platform === "darwin" ? "libtny.1.dylib" : "libtny.so.1"));
    copyFileSync(native.executorPath, join(build, "tny-executor"));
    writeFileSync(join(build, "manifest.json"), JSON.stringify({ ...native.manifest, schemaVersion: 2 }));
    const call = (name, args) => `tools.call(${JSON.stringify(name)}, ${JSON.stringify(JSON.stringify(args))})`;
    const write = `print(${call("write_file", { path: "written", content: "native write" })})`;
    const success = [
      "import pathlib, subprocess, json, math",
      "assert math.fmod(7, 2) == 1 and math.hypot(3, 4) == 5",
      write,
      `print(${call("read_file", { path: "written" })})`,
      `subprocess.run(["/bin/sh", "-c", "printf 'native shell' > shell"], check=True)`,
      "with open('count', 'a') as f: f.write('1')",
      "catalog = {item['function']['name'] for item in json.loads(tools.list())}",
      "assert 'terminal' not in catalog and 'subagent' not in catalog, catalog",
      `assert 'error' in ${call("terminal", { command: "touch forbidden" })}.lower()`,
      `assert 'error' in ${call("subagent", { action: "create", prompt: "no" })}.lower()`,
      "print('restricted surface checked')",
    ].join("\n");
    const cancel = "import time, pathlib\npathlib.Path('started').write_text('yes')\ntime.sleep(2)\npathlib.Path('late').write_text('bad')";
    const mockScript = resolve(here, "../../../tests/integration/mock_openai.py");
    for (const [kind, wire, code] of [
      ["success", "responses", success], ["success", "chat", success],
      ["deny", "responses", write], ["disabled", "responses", write],
      ["cancel", "responses", cancel],
    ]) {
      const workspace = join(root, `${kind}-${wire}`);
      mkdirSync(join(workspace, "empty-path"), { recursive: true });
      await withMock(mockScript, {
        MOCK_EXPECT_WIRE: wire, MOCK_CUSTOM_TOOL: "run_code",
        MOCK_CUSTOM_ARGUMENTS: JSON.stringify({ code, timeout_ms: 10_000 }),
      }, async (baseUrl) => {
        const result = await new Promise((done, reject) => {
          const child = spawn(process.execPath, [fileURLToPath(import.meta.url), "--case"], {
            env: { ...process.env, PATH: join(workspace, "empty-path"),
              TNY_NATIVE_PACKAGE_DIR: "", TNY_TEST_STAGE: stage, TNY_TEST_WORKSPACE: workspace,
              TNY_TEST_CASE: kind, TNY_TEST_WIRE: wire, TNY_TEST_BASE_URL: baseUrl },
            cwd: workspace, stdio: ["ignore", "pipe", "pipe"],
          });
          let stdout = "", stderr = "";
          child.stdout.on("data", (data) => { stdout += data; });
          child.stderr.on("data", (data) => { stderr += data; });
          const timer = setTimeout(() => child.kill("SIGKILL"), 45_000);
          child.once("error", (error) => { clearTimeout(timer); reject(error); });
          child.once("exit", (status, signal) => { clearTimeout(timer); done({ status, signal, stdout, stderr }); });
        });
        assert.equal(result.status, 0, `${kind}/${wire}: ${result.signal || ""}\n${result.stderr}\n${result.stdout}`);
        process.stdout.write(result.stdout);
      });
    }
    console.log("Native library acceptance passed outside checkout with no tny CLI on PATH");
  } finally { rmSync(root, { recursive: true, force: true }); }
}

if (process.argv.includes("--case")) await childCase();
else await main();
