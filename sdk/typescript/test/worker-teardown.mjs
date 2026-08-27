import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, readdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import { spawnSync } from "node:child_process";
import { Worker } from "node:worker_threads";

import { Runtime, sdkPackageRoot } from "./sdk.mjs";

const sdkUrl = pathToFileURL(join(sdkPackageRoot, "dist/index.mjs")).href;
const addonPath = join(sdkPackageRoot, "build/Release/tny.node");
const threadCount = () => {
  if (process.platform === "linux") return readdirSync("/proc/self/task").length;
  if (process.platform !== "darwin")
    throw new Error(`thread counting is unsupported on ${process.platform}`);
  const result = spawnSync("ps", ["-M", "-p", String(process.pid)], { encoding: "utf8" });
  if (result.status !== 0)
    throw new Error(`ps thread count failed with status ${result.status ?? "unknown"}`);
  const lines = result.stdout.trim().split("\n");
  if (lines.length < 2) throw new Error("ps returned no thread rows");
  return lines.length - 1;
};

function boundedInteger(name, fallback, minimum, maximum) {
  const raw = process.env[name];
  if (raw === undefined || raw === "") return fallback;
  if (!/^(0|[1-9][0-9]*)$/.test(raw))
    throw new Error(`${name} must be an integer from ${minimum} through ${maximum}`);
  const value = Number(raw);
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum)
    throw new Error(`${name} must be an integer from ${minimum} through ${maximum}`);
  return value;
}

const baselineThreads = threadCount();
const baselineFds = readdirSync("/dev/fd").length;

async function waitForResourceBaseline() {
  const threadLimit = baselineThreads + 3;
  const fdLimit = baselineFds + 6;
  const deadline = Date.now() + 5000;
  let threads;
  let fds;
  do {
    threads = threadCount();
    fds = readdirSync("/dev/fd").length;
    if (threads <= threadLimit && fds <= fdLimit) return;
    await new Promise((resolve) => setTimeout(resolve, 50));
  } while (Date.now() < deadline);
  assert.ok(
    threads <= threadLimit && fds <= fdLimit,
    `worker resources did not return to baseline: threads ${threads}/${threadLimit}, fds ${fds}/${fdLimit}`,
  );
}

function workerSource(withSession) {
  return `
    import { parentPort } from 'node:worker_threads';
    import { mkdtempSync, mkdirSync } from 'node:fs';
    import { tmpdir } from 'node:os';
    import { join } from 'node:path';
    const { Runtime } = await import(${JSON.stringify(sdkUrl)});
    const root = mkdtempSync(join(tmpdir(), 'tny-worker-'));
    const workspace = join(root, 'workspace'); mkdirSync(workspace);
    const runtime = await Runtime.create({workspace,baseUrl:'http://127.0.0.1:1/v1',apiKey:'x'});
    ${withSession ? "await runtime.createSession();" : ""}
    parentPort.postMessage('ready');
  `;
}

async function createAndTerminate(withSession) {
  const worker = new Worker(workerSource(withSession), { eval: true, type: "module" });
  let timer;
  try {
    await new Promise((resolve, reject) => {
      timer = setTimeout(() => reject(new Error("worker runtime setup timed out")), 15000);
      worker.once("message", resolve);
      worker.once("error", reject);
      worker.once("exit", (code) => {
        if (code !== 0) reject(new Error(`worker exited ${code} before ready`));
      });
    });
  } finally {
    clearTimeout(timer);
    let terminationTimer;
    try {
      await Promise.race([
        worker.terminate(),
        new Promise((_, reject) => {
          terminationTimer = setTimeout(
            () => reject(new Error("worker environment teardown timed out")), 15000);
        }),
      ]);
    } catch (error) {
      worker.unref();
      throw error;
    } finally {
      clearTimeout(terminationTimer);
    }
  }
}

async function runBatched(count, concurrency, withSession) {
  let completed = 0;
  for (let offset = 0; offset < count; offset += concurrency) {
    const batch = Math.min(concurrency, count - offset);
    await Promise.all(Array.from({ length: batch }, async () => {
      await createAndTerminate(withSession);
      completed += 1;
    }));
  }
  assert.equal(completed, count, "every requested worker cycle completed");
  return completed;
}

const root = mkdtempSync(join(tmpdir(), "tny-cross-env-"));
const workspace = join(root, "workspace");
mkdirSync(workspace);
const mainRuntime = await Runtime.create({
  workspace, baseUrl: "http://127.0.0.1:1/v1", apiKey: "x",
});
const mainSession = await mainRuntime.createSession();
const foreign = new Worker(`
  import { parentPort } from 'node:worker_threads';
  import { createRequire } from 'node:module';
  const native = createRequire(import.meta.url)(${JSON.stringify(addonPath)});
  try { await native.cancel(1, 1); parentPort.postMessage('unexpected'); }
  catch (error) { parentPort.postMessage(error.status); }
`, { eval: true, type: "module" });
const foreignStatus = await new Promise((resolve, reject) => {
  foreign.once("message", resolve); foreign.once("error", reject);
});
assert.equal(foreignStatus, -2);
await foreign.terminate();
await mainSession.close();
await mainRuntime.close();

const runtimeCycles = boundedInteger("TNY_WORKER_RUNTIME_CYCLES", 200, 0, 100000);
const sessionCycles = boundedInteger("TNY_WORKER_SESSION_CYCLES", 50, 0, 100000);
const concurrency = boundedInteger("TNY_WORKER_CONCURRENCY", 10, 1, 64);
const completedCycles =
  await runBatched(runtimeCycles, concurrency, false) +
  await runBatched(sessionCycles, concurrency, true);
assert.equal(completedCycles, runtimeCycles + sessionCycles);
await waitForResourceBaseline();
console.log(`${completedCycles} worker teardown cycles and cross-environment handle rejection passed`);
