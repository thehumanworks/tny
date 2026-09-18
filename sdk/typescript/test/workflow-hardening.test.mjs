import assert from "node:assert/strict";
import test from "node:test";
import { Workflow, Runtime, WorkflowContextError } from "./sdk.mjs";

const usage = (inputTokens = 7n) => ({ type: "usage", kind: 6, inputTokens, outputTokens: 2n,
  contextUsed: 10n, contextSize: 100n, cost: 0.25, hasCost: true });

test("native failure and cancellation retain last observed and cleanup usage", async () => {
  const original = Runtime.create;
  try {
    for (const mode of ["stream", "close", "cancel"]) {
      const controller = new AbortController();
      let callback;
      let before;
      const workflow = new Workflow({ runtime: {} }).task("a", "p");
      Runtime.create = async () => ({ createSession: async () => ({ id: "s",
        ask: async (_prompt, { onEvent }) => {
          callback = onEvent;
          await onEvent(usage(3n));
          await onEvent(usage());
          before = workflow.partialUsage;
          if (mode === "stream") throw new Error("stream failed");
          if (mode === "cancel") controller.abort("cancel reason");
          return { text: "ok", stopReason: "done", usage: usage() };
        },
        close: async () => {
          if (mode === "cancel") await callback(usage(9n));
          if (mode === "close") throw new Error("close failed");
        },
      }), close: async () => {} });
      if (mode === "cancel") {
        await assert.rejects(workflow.run({ signal: controller.signal }), (e) => e === "cancel reason");
        assert.equal(workflow.partialUsage.inputTokens, 9n);
      } else {
        const result = await workflow.run();
        assert.equal(result.ok, false);
        assert.equal(result.usage.inputTokens, 7n);
        assert.equal(result.usage.cost, 0.25);
      }
      assert.equal(before.inputTokens, 7n);
      assert.equal(workflow.partialUsage.knownTasks, 1);
    }
  } finally { Runtime.create = original; }
});

test("whole-source finite JSON and bounded nesting, last duplicate wins", async () => {
  const invalid = ["[]", "null", "{", '{"keep":1,"omit":' + "[".repeat(130) + "0" + "]".repeat(130) + "}"];
  for (const token of ["NaN", "Infinity", "-Infinity", "1e400", "9".repeat(400)]) {
    invalid.push(`{"keep":${token}}`, `{"keep":1,"omit":${token}}`, `{"keep":1,"omit":${token},"omit":0}`);
  }
  for (const output of [...invalid, '{"keep":1,"keep":2}', '{"keep":9007199254740991}', '{"keep":"1e400"}']) {
    let selectedPrompt;
    const result = await new Workflow({ runner: async (task, prompt) => {
      if (task.name === "b") selectedPrompt = prompt;
      return { output };
    } }).task("a", "p")
      .task("b", "p", { dependsOn: [{ name: "a", context: "fields", fields: ["keep"] }] }).run();
    assert.equal(result.ok, !invalid.includes(output), output);
    if (!result.ok) assert.ok(result.require("b").error instanceof WorkflowContextError);
    if (output === '{"keep":1,"keep":2}') assert.ok(selectedPrompt.includes('"fields":{"keep":2}'));
  }
});

test("each selector honors exact input, dependency and selection-read bounds", async () => {
  const output = '{"keep":"value","omit":42}';
  for (const edge of [{ name: "a", context: "summary", summary: "é" },
    { name: "a", context: "fields", fields: ["keep"] },
    { name: "a", context: "artifact", offset: 1, length: 5 }]) {
    const prompts = new Map();
    const flow = (options = {}) => new Workflow({ ...options, runner: async (task, prompt) => {
      prompts.set(task.name, prompt);
      return { output, sessionId: "session" };
    } }).task("a", "p").task("b", "é", { dependsOn: [edge] });
    assert.equal((await flow().run()).ok, true);
    const prompt = prompts.get("b");
    const selected = prompt.split('<dependency name="a">\n')[1].split("\n</dependency>")[0];
    const bounds = { maxInputBytes: Buffer.byteLength(prompt), maxDependencyBytes: Buffer.byteLength(selected) };
    if (edge.context !== "summary") bounds.maxSelectionBytes = edge.context === "fields" ? Buffer.byteLength(output) : 5;
    for (const [option, bound] of Object.entries(bounds)) {
      assert.equal((await flow({ [option]: bound }).run()).ok, true);
      prompts.delete("b");
      assert.equal((await flow({ [option]: bound - 1 }).run()).ok, false);
      assert.equal(prompts.has("b"), false);
    }
  }
});

test("mixed fan-in includes provenance and base64 at exact complete-input bound", async () => {
  const prompts = new Map();
  const edges = [{ name: "raw" }, { name: "summary", context: "summary", summary: "é" },
    { name: "fields", context: "fields", fields: ["keep"] },
    { name: "artifact", context: "artifact", offset: 1, length: 5 }, { name: "none", includeOutput: false }];
  function flow(maxInputBytes) {
    const w = new Workflow({ maxInputBytes, runner: async (task, prompt) => {
      prompts.set(task.name, prompt);
      return { output: '{"keep":"value","omit":42}', sessionId: "session" };
    } });
    for (const edge of edges) w.task(edge.name, "p");
    return w.task("consumer", "é", { dependsOn: edges });
  }
  assert.equal((await flow(10000).run()).ok, true);
  const prompt = prompts.get("consumer");
  assert.ok(prompt.includes('"data_base64":"ImtlZXA="'));
  assert.ok(prompt.includes('"session_base64":"c2Vzc2lvbg=="'));
  const bound = Buffer.byteLength(prompt);
  assert.equal((await flow(bound).run()).ok, true);
  prompts.delete("consumer");
  assert.equal((await flow(bound - 1).run()).ok, false);
  assert.equal(prompts.has("consumer"), false);
});

test("inline artifact slice reaches native HTTP request in a distinct workspace", { timeout: 30000 }, async () => {
  const { createServer } = await import("node:http");
  const { mkdtemp, mkdir, rm } = await import("node:fs/promises");
  const { tmpdir } = await import("node:os");
  const { join } = await import("node:path");
  const root = await mkdtemp(join(tmpdir(), "tny-inline-"));
  const workspace = join(root, "isolated");
  await mkdir(workspace);
  const consumerWorkspace = join(root, "consumer-isolated");
  await mkdir(consumerWorkspace);
  const requests = [];
  const server = createServer(async (request, response) => {
    let body = "";
    for await (const chunk of request) body += chunk;
    requests.push(JSON.parse(body));
    const text = requests.length === 1 ? "portable-slice" : "received";
    const events = [
      { type: "response.created", response: { status: "in_progress" } },
      { type: "response.output_item.added", output_index: 0,
        item: { type: "message", id: "msg_1", role: "assistant", content: [] } },
      { type: "response.output_text.delta", item_id: "msg_1", output_index: 0, delta: text },
      { type: "response.output_text.done", item_id: "msg_1", output_index: 0, text },
      { type: "response.completed", response: { status: "completed", usage: { input_tokens: 7, output_tokens: 2 } } },
    ];
    response.writeHead(200, { "Content-Type": "text/event-stream" });
    response.end(events.map((event) => `event: ${event.type}\ndata: ${JSON.stringify(event)}\n\n`).join(""));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  try {
    const result = await new Workflow({ runtime: { workspace, stateDir: join(root, "state"),
      baseUrl: `http://127.0.0.1:${server.address().port}/v1`, apiKey: "fixture-not-secret" },
    }).task("source", "produce").task("consumer", "use inline slice", {
      runtime: { workspace: consumerWorkspace, stateDir: join(root, "consumer-state"),
        baseUrl: `http://127.0.0.1:${server.address().port}/v1`, apiKey: "fixture-not-secret" },
      dependsOn: [{ name: "source", context: "artifact", offset: 0, length: 8 }],
    }).run();
    assert.equal(result.ok, true);
    assert.equal(requests.length, 2);
    assert.notEqual(workspace, process.cwd());
    assert.notEqual(consumerWorkspace, workspace);
    const wire = JSON.stringify(requests[1]);
    assert.ok(wire.includes(Buffer.from("portable").toString("base64")));
    assert.ok(wire.includes("data_base64"));
    assert.ok(wire.includes(result.require("source").artifact.sha256));
    assert.equal(result.usage.inputTokens, 14n);
    const failed = await new Workflow({ runtime: { workspace, stateDir: join(root, "state"),
      baseUrl: `http://127.0.0.1:${server.address().port}/v1`, apiKey: "fixture-not-secret" },
      onEvent: (_task, event) => { if (event.type === "usage") throw new Error("after native usage"); },
    }).task("failed", "fail after usage").run();
    assert.equal(failed.ok, false);
    assert.equal(failed.usage.inputTokens, 7n);
    assert.equal(failed.usage.outputTokens, 2n);
    assert.equal(failed.require("failed").error.message, "after native usage");
  } finally {
    await new Promise((resolve) => server.close(resolve));
    await rm(root, { recursive: true, force: true });
  }
});
