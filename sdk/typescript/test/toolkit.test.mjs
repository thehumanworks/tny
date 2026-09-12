import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, readdirSync, realpathSync, rmSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { inspect } from "node:util";
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

/** Staging files left beside an output, which must always be none. Records are
 * the documented new default artifact and are checked separately. */
function staging(workspace, stem) {
  return readdirSync(workspace)
    .filter(name => name.startsWith(stem + ".") && !name.includes(".tny-image-"));
}

function records(workspace, stem) {
  return readdirSync(workspace)
    .filter(name => name.startsWith(stem + ".tny-image-") && name.endsWith(".json"))
    .map(name => JSON.parse(readFileSync(join(workspace, name), "utf8")));
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
  // Dimensions come from the returned bytes; no size was requested.
  assert.equal(result.width, 1);
  assert.equal(result.height, 1);
  assert.equal(result.requestedSize, "auto");
  assert.equal(result.effectiveSize, "auto");
  assert.equal(result.sizeStatus, "auto");
  assert.ok(Object.isFrozen(result));
  assert.deepEqual(readFileSync(result.path), Buffer.from(f.assets.png, "base64"));
  await f.toolkit.editImage("blue", { outputFile: "out.png", images: ["out.png"] });
  const requests = await f.requests();
  assert.equal(requests.at(-1).path, "/backend-api/codex/images/edits");
  assert.match(requests.at(-1).body.images[0].image_url, /^data:image\/png;base64,/);
  assert.equal(process.cwd(), cwd);
  assert.ok(!existsSync(join(f.workspace, ".tny")));
  // No staging copy survives; the per-operation records do.
  assert.deepEqual(staging(f.workspace, "out.png"), []);
  assert.equal(records(f.workspace, "out.png").length, 2);
});

test("toolkit records manifests, lineage and the persistence opt-out", async t => {
  const f = await fixture(t);
  const result = await f.toolkit.generateImage("a tree", { outputFile: "out.png" });
  assert.ok(result.manifestPath);
  assert.match(result.operationId, /^[0-9a-f]{16}$/);
  const record = JSON.parse(readFileSync(result.manifestPath, "utf8"));
  assert.equal(record.version, 1);
  assert.equal(record.kind, "image_manifest");
  assert.equal(record.status, "succeeded");
  assert.equal(record.prompt, "a tree");
  assert.equal(record.operation_id, result.operationId);
  assert.equal(record.artifacts.length, 1);
  assert.equal(record.artifacts[0].role, "native");
  // Absent provider identifiers stay absent rather than being invented.
  assert.equal(result.seed, null);
  assert.equal(result.requestId, null);
  assert.equal(record.actual.seed, null);
  assert.equal(record.actual.request_id, null);
  // An earlier record's verified output becomes a reference; its source is kept.
  const edited = await f.toolkit.editImage("make it blue", {
    outputFile: "edit.png", artifact: result.manifestPath,
  });
  const edit = JSON.parse(readFileSync(edited.manifestPath, "utf8"));
  assert.equal(edit.references.length, 1);
  assert.equal(edit.references[0].source_manifest, result.manifestPath);
  assert.equal(edit.references[0].source_operation, result.operationId);
  // A rerun reuses the recorded prompt with none supplied.
  const rerun = await f.toolkit.generateImage(undefined, {
    outputFile: "again.png", fromManifest: result.manifestPath,
  });
  const requests = await f.requests();
  assert.equal(requests.at(-1).body.prompt, "a tree");
  assert.equal(
    JSON.parse(readFileSync(rerun.manifestPath, "utf8")).source.operation_id,
    result.operationId,
  );
  // The opt-out records nothing, and never the prompt.
  const quiet = await f.toolkit.generateImage("a private tree", {
    outputFile: "quiet.png", persistManifest: false,
  });
  assert.equal(quiet.manifestPath, null);
  assert.deepEqual(readdirSync(f.workspace).filter(n => n.startsWith("quiet.png.")), []);
  for (const entry of readdirSync(f.workspace, { withFileTypes: true })) {
    if (!entry.isFile()) continue;
    assert.ok(!readFileSync(join(f.workspace, entry.name)).includes("a private tree"), entry.name);
  }
  await f.mode("identified");
  const identified = await f.toolkit.generateImage("a tree", { outputFile: "id.png" });
  assert.equal(identified.seed, f.assets.seed);
  assert.equal(identified.requestId, f.assets.requestId);
  const kept = JSON.parse(readFileSync(identified.manifestPath, "utf8"));
  assert.deepEqual(kept.actual, { seed: f.assets.seed, request_id: f.assets.requestId });
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
    () => f.toolkit.generateImage("tree", { outputFile: "out.png", preview: true }),
    () => f.toolkit.generateImage("tree", { outputFile: "out.png", preview: false }),
    () => f.toolkit.editImage("tree", { outputFile: "out.png", images: ["in.png"], preview: true }),
    () => f.toolkit.generateImage("tree", { outputFile: "out.png", strictSize: "yes" }),
    () => f.toolkit.generateImage("tree", { outputFile: "out.png", strict_size: true }),
    // Strict size without an exact WIDTHxHEIGHT never reaches the provider.
    () => f.toolkit.generateImage("tree", { outputFile: "out.png", strictSize: true }),
    () => f.toolkit.generateImage("tree", { outputFile: "out.png", size: "auto", strictSize: true }),
    () => f.toolkit.editImage("edit", { outputFile: "out.png", images: [] }),
    () => f.toolkit.dictate({ seconds: true }),
    () => f.toolkit.dictate({ seconds: 1.5 }),
    () => f.toolkit.dictate({ seconds: 301 }),
    () => f.toolkit.optimise("text", { timeoutSeconds: 0 }),
  ]) await assert.rejects(run);
  assert.equal((await f.requests()).length, 0);
});

test("strict size rejects a mismatch without replacing the output", async t => {
  const f = await fixture(t);
  const output = join(f.workspace, "out.png");
  writeFileSync(output, "preserve");
  // The fixture image is 1x1, so an exact 1024x1024 request cannot be met.
  await assert.rejects(
    f.toolkit.generateImage("tree", { outputFile: "out.png", size: "1024x1024", strictSize: true }),
    error => {
      assert.ok(error instanceof TnyError);
      assert.equal(error.status, -10);
      // The locally decided reason travels with the same generic error.
      assert.deepEqual(error.imageDetail, {
        code: "IMAGE_SIZE_MISMATCH", operation: "generate",
        message: error.imageDetail.message, requestedSize: "1024x1024",
        effectiveSize: "1024x1024", width: 1, height: 1, sizeStatus: "mismatch",
        mimeType: "image/png", path: null, committed: false,
      });
      assert.ok(Object.isFrozen(error.imageDetail));
      assert.match(error.imageDetail.message, /^IMAGE_SIZE_MISMATCH: /);
      // Normal formatting, enumeration and serialization show nothing of it.
      for (const text of [error.message, error.stack, String(error),
                          JSON.stringify(error), inspect(error)]) {
        assert.ok(!text.includes("1024x1024"), text);
        assert.ok(!text.includes("IMAGE_SIZE_MISMATCH"), text);
        assert.ok(!text.includes(f.assets.token), text);
      }
      assert.ok(!Object.keys(error).includes("imageDetail"));
      assert.ok(!Object.keys(error).includes("imageDetailJson"));
      assert.throws(() => { error.imageDetail = null; }, TypeError);
      return true;
    },
  );
  assert.equal(readFileSync(output, "utf8"), "preserve");
  assert.equal((await f.requests()).length, 1); // paid once, never retried
  const result = await f.toolkit.generateImage("tree", { outputFile: "out.png", size: "1x1", strictSize: true });
  assert.equal(result.sizeStatus, "match");
  assert.equal(result.requestedSize, "1x1");
  assert.equal(result.effectiveSize, "1x1");
  assert.deepEqual(readFileSync(output), Buffer.from(f.assets.png, "base64"));
});

test("strict size rejected before any request still explains itself", async t => {
  const f = await fixture(t);
  await assert.rejects(
    f.toolkit.generateImage("tree", { outputFile: "out.png", size: "portrait", strictSize: true }),
    error => {
      assert.equal(error.status, -1);
      assert.equal(error.imageDetail.code, "IMAGE_STRICT_SIZE_INVALID");
      assert.equal(error.imageDetail.requestedSize, "portrait");
      // Nothing was sent, so no wire size and no dimensions are invented.
      assert.equal(error.imageDetail.effectiveSize, null);
      assert.equal(error.imageDetail.width, null);
      assert.equal(error.imageDetail.height, null);
      assert.equal(error.imageDetail.mimeType, null);
      assert.equal(error.imageDetail.committed, false);
      return true;
    },
  );
  assert.equal((await f.requests()).length, 0);
  assert.ok(!existsSync(join(f.workspace, "out.png")));
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
    // A provider failure is never labelled as safe local detail.
    assert.equal(error.imageDetail, undefined);
    assert.equal(error.imageDetailJson, undefined);
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
  const rejected = assert.rejects(call, error => error instanceof TnyError && error.status === -12 &&
    error.imageDetail === undefined);
  await arrived(f, before + 1);
  controller.abort();
  await rejected;
  assert.equal(readFileSync(output, "utf8"), "preserve");
  assert.deepEqual(staging(f.workspace, "out.png"), []);
  // Every record left behind is terminal, never a live-looking intent.
  for (const record of records(f.workspace, "out.png")) {
    assert.ok(["failed", "cancelled"].includes(record.status), record.status);
    assert.equal(record.committed, false);
  }
});

test("both image failure detail types are declared and re-exported", () => {
  // Types vanish at runtime, so the declarations themselves are the surface.
  const declared = readFileSync(join(sdkPackageRoot, "dist/toolkit.d.ts"), "utf8");
  for (const fragment of [
    "export interface ImageFailureDetail",
    "export interface RetainedImageDetail",
    "export type ImageDetail = ImageFailureDetail | RetainedImageDetail;",
    "readonly imageDetail?: ImageDetail;",
    'readonly code: "IMAGE_MANIFEST_FINALIZE_FAILED";',
    "readonly committed: true;",
  ]) assert.ok(declared.includes(fragment), fragment);
  const index = readFileSync(join(sdkPackageRoot, "dist/index.d.ts"), "utf8");
  assert.ok(index.includes('export * from "./toolkit.js";'));
});

test("a failed manifest finalization keeps the paid image and says so", async t => {
  const f = await fixture(t);
  const output = join(f.workspace, "out.png");
  // The fixture holds the response while the running record already exists and
  // the output is not yet committed, so only the finalizing rename can fail.
  // Nothing is mocked: the record really becomes a directory on disk.
  await f.mode("stall");
  const call = f.toolkit.generateImage("tree", { outputFile: "out.png" });
  await arrived(f);
  let running;
  for (let n = 0; n < 200 && !running; n++) {
    [running] = readdirSync(f.workspace)
      .filter(name => name.startsWith("out.png.tny-image-") && name.endsWith(".json"));
    if (!running) await delay(10);
  }
  assert.ok(running, "the running record must exist before the response");
  rmSync(join(f.workspace, running));
  mkdirSync(join(f.workspace, running));
  // The held successful response must actually carry provider metadata.
  await f.mode("identified");
  await assert.rejects(call, error => {
    assert.ok(error instanceof TnyError);
    assert.equal(error.status, -7); // I/O, not a strict rejection
    assert.equal(typeof error.imageDetailJson, "string");
    const raw = JSON.parse(error.imageDetailJson);
    assert.equal(raw.committed, true);
    assert.equal(Object.hasOwn(raw, "seed"), false);
    assert.equal(Object.hasOwn(raw, "request_id"), false);
    assert.ok(!error.imageDetailJson.includes(f.assets.requestId));
    assert.deepEqual(error.imageDetail, {
      code: "IMAGE_MANIFEST_FINALIZE_FAILED", operation: "generate",
      message: error.imageDetail.message, path: output,
      byteCount: Buffer.from(f.assets.png, "base64").length,
      requestedSize: "auto", effectiveSize: "auto", width: 1, height: 1,
      sizeStatus: "auto", mimeType: "image/png",
      operationId: running.slice("out.png.tny-image-".length, -".json".length),
      manifestPath: join(f.workspace, running), committed: true,
    });
    assert.ok(Object.isFrozen(error.imageDetail));
    // Normal formatting, enumeration and serialization show nothing of it.
    for (const text of [error.message, error.stack, String(error),
                        JSON.stringify(error), inspect(error)]) {
      assert.ok(!text.includes("IMAGE_MANIFEST_FINALIZE_FAILED"), text);
      assert.ok(!text.includes(f.assets.token), text);
      assert.ok(!text.includes("tree"), text);
    }
    assert.ok(!Object.keys(error).includes("imageDetail"));
    assert.throws(() => { error.imageDetail = null; }, TypeError);
    return true;
  });
  // The paid artifact is exactly the provider's bytes, and kept.
  assert.deepEqual(readFileSync(output), Buffer.from(f.assets.png, "base64"));
  assert.deepEqual(staging(f.workspace, "out.png"), []);
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
  assert.deepEqual(staging(f.workspace, "worker.png"), []);
  // A terminated worker leaves no record claiming a committed artifact.
  for (const record of records(f.workspace, "worker.png")) {
    assert.equal(record.committed, false);
    assert.deepEqual(record.artifacts, []);
  }
});
