import assert from "node:assert/strict";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import test from "node:test";

test("throwing option getter has no native side effects", () => {
  const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
  const addon = join(packageRoot, "build/Release/tny.node");
  const source = `
    import { createRequire } from 'node:module';
    import { mkdtempSync, mkdirSync } from 'node:fs';
    import { tmpdir } from 'node:os';
    import { join } from 'node:path';
    const native = createRequire(import.meta.url)(${JSON.stringify(addon)});
    const root = mkdtempSync(join(tmpdir(), 'tny-accessor-'));
    const workspace = join(root, 'workspace'); mkdirSync(workspace);
    const base = {workspace,stateDir:'',provider:'openai',model:'',baseUrl:'http://127.0.0.1:1/v1',apiKey:'x',wireApi:'',permissionMode:0,persistence:false,maxToolResultBytes:32768n};
    Object.defineProperty(base, 'maxSteps', { enumerable:true, get(){ throw new Error('getter exploded'); } });
    try { await native.createRuntime(base); throw new Error('unexpected success'); }
    catch (error) { if (error.message !== 'getter exploded') throw error; }
    const valid = {workspace,stateDir:'',provider:'openai',model:'',baseUrl:'http://127.0.0.1:1/v1',apiKey:'x',wireApi:'',permissionMode:0,persistence:false,maxSteps:0,maxToolResultBytes:32768n};
    const runtime = await native.createRuntime(valid);
    await native.closeRuntime(runtime.runtimeId);
  `;
  const run = spawnSync(process.execPath, ["--input-type=module"], {
    input: source, encoding: "utf8",
  });
  assert.equal(run.status, 0, run.stderr);
});
