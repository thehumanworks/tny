import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { Runtime } from "./sdk.mjs";

const timers = new Set();
const server = createServer((request, response) => {
  request.resume();
  request.on("end", () => {
    response.writeHead(200, { "content-type": "text/event-stream" });
    response.flushHeaders();
    const timer = setTimeout(() => response.end(), 5000);
    timers.add(timer);
    response.once("close", () => { clearTimeout(timer); timers.delete(timer); });
  });
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const root = mkdtempSync(join(tmpdir(), "tny-wake-cancel-"));
const workspace = join(root, "workspace");
mkdirSync(workspace);
const runtime = await Runtime.create({
  workspace, baseUrl: `http://127.0.0.1:${server.address().port}/v1`, apiKey: "test-only",
});
const session = await runtime.createSession();
const iterator = session.run("block in next_event");
let pending = iterator.next();
await new Promise((resolve) => setTimeout(resolve, 100));
const started = performance.now();
await session.cancel();
const events = [];
for (;;) {
  const item = await pending;
  if (item.done) break;
  events.push(item.value);
  pending = iterator.next();
}
assert.ok(performance.now() - started < 1000, "direct cancel woke blocked next_event promptly");
assert.equal(events.at(-1)?.type, "turn_end");
assert.equal(events.at(-1)?.stopReason, "interrupted");
await session.close();
await runtime.close();
for (const timer of timers) clearTimeout(timer);
await new Promise((resolve) => server.close(resolve));
console.log("cross-thread cancel woke a blocked five-second next_event");
