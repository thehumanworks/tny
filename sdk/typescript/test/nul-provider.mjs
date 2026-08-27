import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { Runtime } from "./sdk.mjs";

const text = "A\0B";
const events = [
  { type: "response.created", response: { status: "in_progress" } },
  {
    type: "response.output_item.added", output_index: 0,
    item: { type: "message", id: "msg_nul", role: "assistant", content: [] },
  },
  { type: "response.output_text.delta", item_id: "msg_nul", output_index: 0, delta: text },
  { type: "response.output_text.done", item_id: "msg_nul", output_index: 0, text },
  {
    type: "response.completed",
    response: { status: "completed", usage: { input_tokens: 1, output_tokens: 1 } },
  },
];
const server = createServer((request, response) => {
  request.resume();
  request.on("end", () => {
    response.writeHead(200, { "content-type": "text/event-stream" });
    for (const event of events)
      response.write(`event: ${event.type}\ndata: ${JSON.stringify(event)}\n\n`);
    response.end();
  });
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const port = server.address().port;
const root = mkdtempSync(join(tmpdir(), "tny-nul-provider-"));
const workspace = join(root, "workspace");
mkdirSync(workspace);
const runtime = await Runtime.create({
  workspace, baseUrl: `http://127.0.0.1:${port}/v1`, apiKey: "test-only",
});
const session = await runtime.createSession();
const answer = await session.ask("return nul");
assert.equal(answer.text.length, 3);
assert.deepEqual([...answer.text].map((value) => value.charCodeAt(0)), [65, 0, 66]);
await session.close();
await runtime.close();
await new Promise((resolve) => server.close(resolve));
console.log("provider NUL text preserved with explicit length");
