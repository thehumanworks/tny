import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createRequire } from "node:module";

for (const key of [
  "runtimeId", "abiVersion", "libraryVersion", "capabilities", "sessionHandle",
  "sessionId", "done", "value", "schemaVersion", "providerSelected",
]) {
  const descriptor = Object.create(null);
  descriptor.configurable = true;
  descriptor.set = () => { throw new Error(`hostile setter ${key}`); };
  Object.defineProperty(Object.prototype, key, descriptor);
}
const { Runtime, sdkPackageRoot } = await import("./sdk.mjs");
const root = mkdtempSync(join(tmpdir(), "tny-hostile-prototype-"));
const workspace = join(root, "workspace");
mkdirSync(workspace);
const native = createRequire(import.meta.url)(join(sdkPackageRoot, "build/Release/tny.node"));
const nativeInfo = await native.createRuntime({
  workspace, stateDir: "", provider: "openai", model: "", baseUrl: "http://127.0.0.1:1/v1",
  apiKey: "test-only", wireApi: "", permissionMode: 0, persistence: false,
  maxSteps: 0, maxToolResultBytes: 32768n,
});
assert.equal(Object.getPrototypeOf(nativeInfo), null);
assert.equal(Object.getPrototypeOf(nativeInfo.capabilities), null);
await native.closeRuntime(nativeInfo.runtimeId);
const runtime = await Runtime.create({
  workspace, baseUrl: "http://127.0.0.1:1/v1", apiKey: "test-only",
});
assert.equal(Object.getPrototypeOf(runtime.capabilities), Object.prototype);
const session = await runtime.createSession();
assert.ok(session.id);
await session.close();
await runtime.close();
console.log("hostile Object.prototype setters did not intercept native results");
