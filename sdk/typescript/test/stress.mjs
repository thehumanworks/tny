import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Runtime } from "./sdk.mjs";

function options() {
  const root = mkdtempSync(join(tmpdir(), "tny-close-stress-"));
  const workspace = join(root, "workspace");
  mkdirSync(workspace);
  return { workspace, baseUrl: "http://127.0.0.1:1/v1", apiKey: "test-only" };
}

const runtime = await Runtime.create(options());
const session = await runtime.createSession();
const pressure = Array.from({ length: 2500 }, () => session.cancel());
const closing = runtime.close();
for (let index = 0; index < 2500; index++) pressure.push(session.cancel());
const results = await Promise.allSettled([...pressure, closing]);
assert.equal(results.at(-1)?.status, "fulfilled");
assert.equal(runtime.closed, true);
const replacement = await Runtime.create(options());
const replacementSession = await replacement.createSession();
await replacementSession.close();
await replacement.close();
console.log("5000 concurrent cancels plus priority close and runtime recreation passed");
