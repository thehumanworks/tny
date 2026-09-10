import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, readdirSync, realpathSync, rmSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { Worker } from "node:worker_threads";
import test from "node:test";
import { Toolkit, TnyError, sdkPackageRoot } from "./sdk.mjs";

const fixturePath = fileURLToPath(new URL("../../../tests/fixtures/toolkit_provider.py", import.meta.url));

async function fixture(t) {
  const child = spawn(process.env.PYTHON || "python3", [fixturePath], { stdio: ["pipe", "pipe", "pipe"] });
  const ready = await Promise.race([
    once(child.stdout, "data").then(([data]) => data.toString().trim()),
    once(child, "exit").then(() => { throw new Error("toolkit fixture exited before startup"); }),
  ]);
  t.after(async () => {
    const exit = once(child, "exit");
    child.stdin.end();
    await exit;
  });
  const assets = await (await fetch(`${ready}/assets`)).json();
  const workspace = realpathSync(mkdtempSync(join(tmpdir(), "tny-node-toolkit-")));
  t.after(() => rmSync(workspace, { recursive: true, force: true }));
  mkdirSync(join(workspace, "src"));
  writeFileSync(join(workspace, "src/context.txt"), "UTF-8 fixture context\n");
  writeFileSync(join(workspace, "settings.json"), "{}");
  writeFileSync(join(workspace, "reference.png"), Buffer.from(assets.png, "base64"));
  writeFileSync(join(workspace, "input.wav"), Buffer.from(assets.wav, "base64"));
  const config = {
    workspace, settingsPath: "settings.json", chatgptToken: assets.token,
    chatgptAccountId: assets.account, codexBaseUrl: `${ready}/backend-api/codex`,
  };
  return {
    workspace, config, assets, toolkit: new Toolkit(config),
    optimise: { provider: "openai", baseUrl: `${ready}/v1`, apiKey: assets.optimiseToken, wireApi: "chat", model: "fixture-model" },
    mode: async (mode) => { await fetch(`${ready}/mode`, { method: "POST", body: JSON.stringify({ mode }) }); },
    requests: async () => (await fetch(`${ready}/requests`)).json(),
  };
}

async function arrived(f, count = 1) {
  for (let n = 0; n < 200; n++) {
    if ((await f.requests()).length >= count) return;
    await delay(10);
  }
  throw new Error("native toolkit request did not arrive");
}

test("toolkit generates and edits real artifacts in its workspace", async t => {
  const f = await fixture(t);
  const cwd = process.cwd();
  const result = await f.toolkit.generateImage("tree", { outputFile: "out.png", model: "fixture-image", quality: "max" });
  assert.equal(result.path, join(f.workspace, "out.png"));
  assert.equal(result.mimeType, "image/png");
  assert.equal(result.byteCount, Buffer.from(f.assets.png, "base64").length);
  assert.equal(result.model, "fixture-image");
  assert.ok(Object.isFrozen(result));
  assert.deepEqual(readFileSync(result.path), Buffer.from(f.assets.png, "base64"));
  await f.toolkit.editImage("blue", { outputFile: "out.png", images: ["out.png"] });
  const requests = await f.requests();
  assert.equal(requests.at(-1).path, "/backend-api/codex/images/edits");
  assert.match(requests.at(-1).body.images[0].image_url, /^data:image\/png;base64,/);
  assert.equal(process.cwd(), cwd);
  assert.ok(!existsSync(join(f.workspace, ".tny")));
});

test("toolkit speech and transcription use independent media credentials", async t => {
  const f = await fixture(t);
  const [speech, transcript] = await Promise.all([
    f.toolkit.speak("hello", { outputFile: "out.mp3", voice: "cove" }),
    f.toolkit.transcribe("input.wav", { provider: "codex" }),
  ]);
  assert.equal(speech.played, false);
  assert.deepEqual(readFileSync(speech.path), Buffer.from(f.assets.mp3, "base64"));
  assert.equal(transcript.text, f.assets.text);
  assert.equal(transcript.provider, "codex");
  assert.ok((await f.requests()).find(r => r.body.wav));
});

test("toolkit optimisation reads project context and cannot execute the draft", async t => {
  const f = await fixture(t);
  await f.mode("explore");
  const result = await f.toolkit.optimise("Improve context", f.optimise);
  assert.deepEqual(result, { text: f.assets.text, model: "fixture-model", provider: "openai" });
  const requests = await f.requests();
  assert.equal(requests.length, 2);
  assert.deepEqual(new Set(requests[0].body.tools.map(tool => tool.function.name)), new Set([
    "read_file", "list_files", "glob_files", "grep_files", "file_info", "read_tool_result",
  ]));
  assert.match(JSON.stringify(requests[1].body.messages), /UTF-8 fixture context/);
  await f.mode("write");
  await f.toolkit.optimize("Improve context", f.optimise);
  assert.equal(readFileSync(join(f.workspace, "src/context.txt"), "utf8"), "UTF-8 fixture context\n");
});

test("toolkit snapshots options and keeps credentials out of inspection", async t => {
  const f = await fixture(t);
  let reads = 0;
  const toolkit = new Toolkit({ ...f.config, get chatgptToken() { reads++; return f.assets.token; } });
  f.config.chatgptToken = "incorrect";
  await toolkit.generateImage("tree", { outputFile: "out.png" });
  assert.equal(reads, 1);
  assert.ok(!JSON.stringify(toolkit).includes(f.assets.token));
});

test("toolkit rejects invalid requests before provider I/O", async t => {
  const f = await fixture(t);
  for (const run of [
    () => f.toolkit.generateImage("", { outputFile: "out.png" }),
    () => f.toolkit.generateImage("bad\0text", { outputFile: "out.png" }),
    () => f.toolkit.generateImage("\ud800", { outputFile: "out.png" }),
    () => f.toolkit.generateImage("tree", { outputFile: "out.png", quality: "impossible" }),
    () => f.toolkit.editImage("edit", { outputFile: "out.png", images: [] }),
    () => f.toolkit.dictate({ seconds: true }),
    () => f.toolkit.dictate({ seconds: 1.5 }),
    () => f.toolkit.dictate({ seconds: 301 }),
    () => f.toolkit.optimise("text", { timeoutSeconds: 0 }),
  ]) await assert.rejects(run);
  assert.equal((await f.requests()).length, 0);
});

test("toolkit errors and cancellation preserve existing outputs", async t => {
  const f = await fixture(t);
  const output = join(f.workspace, "out.png");
  writeFileSync(output, "preserve");
  await f.mode("error");
  await assert.rejects(f.toolkit.generateImage("private prompt", { outputFile: "out.png" }), error => {
    assert.ok(error instanceof TnyError);
    assert.ok(!String(error).includes(f.assets.token));
    assert.ok(!String(error).includes("private prompt"));
    return true;
  });
  const before = (await f.requests()).length;
  const abort = new AbortController();
  abort.abort();
  await assert.rejects(f.toolkit.speak("hello", { signal: abort.signal }), error => error.status === -12);
  assert.equal((await f.requests()).length, before);
  await f.mode("stall");
  const controller = new AbortController();
  const call = f.toolkit.generateImage("tree", { outputFile: "out.png", signal: controller.signal });
  const rejected = assert.rejects(call, error => error instanceof TnyError && error.status === -12);
  await arrived(f, before + 1);
  controller.abort();
  await rejected;
  assert.equal(readFileSync(output, "utf8"), "preserve");
  assert.deepEqual(readdirSync(f.workspace).filter(name => name.startsWith("out.png.")), []);
});

test("toolkit microphone and playback run through fake host devices", async t => {
  const f = await fixture(t);
  const bin = join(f.workspace, "bin");
  mkdirSync(bin);
  for (const name of ["ffmpeg", "arecord"]) {
    writeFileSync(join(bin, name), `#!${f.assets.python}\nimport signal, sys\nsys.stdout.buffer.write(b'\\0' * 48000)\nsys.stdout.buffer.flush()\nsignal.pause()\n`, { mode: 0o755 });
  }
  for (const name of ["afplay", "ffplay", "mpv", "mpg123"]) {
    writeFileSync(join(bin, name), `#!${f.assets.python}\nimport sys\nassert sys.stdin.buffer.read().startswith(b'ID3')\n`, { mode: 0o755 });
  }
  const oldPath = process.env.PATH;
  process.env.PATH = bin;
  try {
    assert.equal((await f.toolkit.dictate({ seconds: 1, provider: "codex" })).text, f.assets.text);
    const speech = await f.toolkit.speak("hello");
    assert.equal(speech.played, true);
    assert.equal(speech.path, null);
  } finally { process.env.PATH = oldPath; }
});

test("worker termination cancels native toolkit work before environment cleanup", async t => {
  const f = await fixture(t);
  await f.mode("stall");
  const worker = new Worker(`
    const { workerData } = require("node:worker_threads");
    const { pathToFileURL } = require("node:url");
    import(pathToFileURL(workerData.entry)).then(({ Toolkit }) => {
      new Toolkit(workerData.config).generateImage("tree", { outputFile: "worker.png" }).catch(() => {});
    });
  `, { eval: true, workerData: { entry: join(sdkPackageRoot, "dist/index.mjs"), config: f.config } });
  t.after(() => worker.terminate());
  await arrived(f);
  const start = Date.now();
  await worker.terminate();
  assert.ok(Date.now() - start < 5000, "cleanup must cancel the stalled request");
  assert.ok(!existsSync(join(f.workspace, "worker.png")));
  assert.deepEqual(readdirSync(f.workspace).filter(name => name.startsWith("worker.png.")), []);
});
