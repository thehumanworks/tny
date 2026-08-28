import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Runtime } from "./sdk.mjs";

const root = mkdtempSync(join(tmpdir(), "tny-invalid-options-"));
const workspace = join(root, "workspace");
mkdirSync(workspace);
const base = { workspace, baseUrl: "http://127.0.0.1:1/v1", apiKey: "secret-must-not-leak" };
for (const options of [
  { maxSteps: "bad" }, { maxSteps: -1 }, { maxSteps: 1.5 }, { maxSteps: Infinity },
  { maxSteps: NaN }, { maxSteps: 2 ** 40 }, { maxSteps: 2147483648 },
  { maxToolResultBytes: -1 }, { maxToolResultBytes: 1.5 }, { persistence: "yes" },
  { model: "bad\0model" }, { model: "\ud800" },
]) {
  await assert.rejects(Runtime.create({ ...base, ...options }));
}
const diagnosticSecret = "SECRET-MUST-NOT-ENTER-ERROR";
await assert.rejects(
  Runtime.create({ ...base, workspace: join(root, "missing"), apiKey: diagnosticSecret }),
  (error) => !String(error.message).includes(diagnosticSecret) &&
    !String(error.stack).includes(diagnosticSecret),
);
const runtime = await Runtime.create(base);
assert.equal(runtime.capabilities.abiMajor, 1);
const session = await runtime.createSession();
for (const prompt of ["bad\0prompt", "\ud800"]) {
  await assert.rejects(async () => {
    for await (const _ of session.run(prompt)) {}
  });
}
await session.close();
await runtime.close();
console.log("invalid options reject without side effects or secret diagnostics");
