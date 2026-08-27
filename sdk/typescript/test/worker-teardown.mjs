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
  const result = spawnSync("ps", ["-M", "-p", String(process.pid)], { encoding: "utf8" });
  return result.status === 0 ? Math.max(0, result.stdout.trim().split("\n").length - 1) : 0;
};
const baselineThreads = threadCount();
const baselineFds = readdirSync("/dev/fd").length;

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
  await new Promise((resolve, reject) => {
    worker.once("message", resolve);
    worker.once("error", reject);
  });
  await worker.terminate();
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

const runtimeCycles = Number(process.env.TNY_WORKER_RUNTIME_CYCLES || 200);
const sessionCycles = Number(process.env.TNY_WORKER_SESSION_CYCLES || 50);
for (let index = 0; index < runtimeCycles; index++) await createAndTerminate(false);
for (let index = 0; index < sessionCycles; index++) await createAndTerminate(true);
await new Promise((resolve) => setTimeout(resolve, 100));
assert.ok(threadCount() <= baselineThreads + 3, "worker owner threads returned near baseline");
assert.ok(readdirSync("/dev/fd").length <= baselineFds + 6, "worker file descriptors returned near baseline");
console.log(`${runtimeCycles + sessionCycles} worker teardown cycles and cross-environment handle rejection passed`);
