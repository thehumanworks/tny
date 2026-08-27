import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Runtime } from "./sdk.mjs";

assert.equal(typeof global.gc, "function");
const root = mkdtempSync(join(tmpdir(), "tny-gc-"));
const workspace = join(root, "workspace");
mkdirSync(workspace);
const runtime = await Runtime.create({
  workspace, baseUrl: "http://127.0.0.1:1/v1", apiKey: "test-only",
});
async function createAndDrop() {
  await runtime.createSession();
}
await createAndDrop();
let replacement;
for (let attempt = 0; attempt < 200 && !replacement; attempt++) {
  global.gc();
  new Array(1000).fill(attempt);
  await new Promise((resolve) => setTimeout(resolve, 5));
  try { replacement = await runtime.createSession(); } catch {}
}
assert.ok(replacement, "dropped session was finalized and native session closed");
await replacement.close();
await runtime.close();
console.log("session finalizer released native slot while runtime remained live");
