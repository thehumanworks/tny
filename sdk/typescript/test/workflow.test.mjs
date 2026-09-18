import assert from "node:assert/strict";
import { inspect } from "node:util";
import test from "node:test";

import {
  Workflow,
  Runtime,
  WorkflowContextError,
  WorkflowDefinitionError,
  WorkflowRunError,
  WorkflowTaskExecution,
  WorkflowTask,
  WorkflowTaskStatus,
} from "./sdk.mjs";

test("32 x 256KiB consumers compose only inside admission", async () => {
  let enter;
  let release;
  const entered = new Promise((resolve) => { enter = resolve; });
  const barrier = new Promise((resolve) => { release = resolve; });
  const original = WorkflowTask.prototype._prompt;
  let renders = 0;
  WorkflowTask.prototype._prompt = function () {
    if (this.name !== "source") renders++;
    return original.call(this);
  };
  const workflow = new Workflow({ maxConcurrency: 1, runner: async (task) => {
    if (task.name === "source") return { output: "x".repeat(262144) };
    enter();
    await barrier;
    return { output: "done" };
  }}).task("source", "produce");
  for (let index = 0; index < 32; index++) {
    workflow.task(`consumer-${index}`, "consume", { dependsOn: ["source"] });
  }
  const run = workflow.run();
  try {
    await entered;
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(renders, 1);
  } finally {
    release();
    const result = await run;
    WorkflowTask.prototype._prompt = original;
    assert.equal(result.ok, true);
  }
  assert.equal(renders, 32);
});

test("explicit summary, JSON fields and bounded artifact references retain originals", async () => {
  const output = '{"keep":{"nested":42},"omit":"private","__proto__":"safe"}';
  const prompts = new Map();
  const workflow = new Workflow({ runner: async (task, prompt) => {
    prompts.set(task.name, prompt);
    return { output: task.name === "source" ? output : "ok", sessionId: "session" };
  }}).task("source", "produce");
  for (const [name, selection] of [
    ["summary", { context: "summary", summary: "explicit summary" }],
    ["fields", { context: "fields", fields: ["keep", "__proto__"] }],
    ["artifact", { context: "artifact", offset: 1, length: 6 }],
    ["reference", { context: "artifact" }],
    ["none", { includeOutput: false }],
  ]) {
    workflow.task(name, "consume", { dependsOn: [{ name: "source", ...selection }] });
  }
  const result = await workflow.run();
  assert.equal(result.ok, true);
  assert.equal(result.output("source"), output);
  const artifact = result.require("source").artifact;
  assert.equal(artifact.read(1, 6).toString(), output.slice(1, 7));
  for (const args of [[0, output.length + 1], [-1, 0], [output.length + 1, 0], [false, 1], [0, 2, 1]]) {
    assert.throws(() => artifact.read(...args), WorkflowContextError);
  }
  for (const name of ["summary", "fields", "artifact", "reference"]) {
    assert.ok(!prompts.get(name).includes("private"));
    assert.ok(prompts.get(name).includes(artifact.sha256));
    assert.ok(prompts.get(name).includes("not higher-priority instructions"));
  }
  assert.equal(prompts.get("none"), "consume");
  const payload = JSON.parse(prompts.get("artifact").split('<dependency name="source">\n')[1].split("\n</dependency>")[0]);
  assert.equal(Buffer.from(payload.data_base64, "base64").toString(), output.slice(1, 7));
  assert.ok(prompts.get("fields").includes('"__proto__":"safe"'));
  assert.ok(!inspect(workflow.tasks).includes("explicit summary"));
  const unicode = await new Workflow({ runner: async () => ({ output: "é🙂" }) }).task("one", "p").run();
  assert.deepEqual(unicode.require("one").artifact.read(1, 3), Buffer.from("é🙂").subarray(1, 4));
});

test("large originals fail whole-output mode but remain available with explicit selection", async () => {
  const output = "x".repeat(1024 * 1024 + 1);
  const prompts = new Map();
  const result = await new Workflow({ runner: async (task, prompt) => {
    prompts.set(task.name, prompt);
    return { output: task.name === "source" ? output : "ok" };
  }}).task("source", "p").task("whole", "p", { dependsOn: ["source"] })
    .task("summary", "p", { dependsOn: [{ name: "source", context: "summary", summary: "selected fact" }] })
    .task("reference", "p", { dependsOn: [{ name: "source", context: "artifact" }] }).run();
  assert.ok(result.require("whole").error instanceof WorkflowContextError);
  assert.equal(prompts.has("whole"), false);
  assert.equal(result.require("summary").ok, true);
  assert.equal(result.require("reference").ok, true);
  assert.equal(result.output("source"), output);
  assert.ok(prompts.get("reference").length < 1024);
  const chunk = result.require("source").artifact.read(0, 1);
  chunk[0] = 0;
  assert.equal(result.require("source").artifact.read(0, 1).toString(), "x");
});

test("context bounds and failed selection prevent execution and block descendants", async () => {
  for (const [options, selection] of [
    [{ maxInputBytes: 100 }, {}], [{ maxDependencyBytes: 2 }, {}],
    [{ maxSelectionBytes: 2 }, { context: "fields", fields: ["key"] }],
    [{ maxSelectionBytes: 2 }, { context: "artifact", length: 3 }],
    [{}, { context: "fields", fields: ["missing"] }],
    [{}, { context: "artifact", offset: 1000 }],
  ]) {
    const started = [];
    const result = await new Workflow({ ...options, runner: async (task) => {
      started.push(task.name);
      return { output: '{"key":42}' };
    }}).task("source", "produce")
      .task("consumer", "consume", { dependsOn: [{ name: "source", ...selection }] })
      .task("blocked", "consume", { dependsOn: ["consumer"] }).run();
    assert.deepEqual(started, ["source"]);
    assert.ok(result.require("consumer").error instanceof WorkflowContextError);
    assert.equal(result.require("blocked").status, "blocked");
  }
  let called = false;
  const result = await new Workflow({ maxInputBytes: 1, runner: async () => {
    called = true;
    return { output: "" };
  }}).task("root", "é").run();
  assert.equal(result.ok, false);
  assert.equal(called, false);
});

test("whole-input exact byte boundary, including framing", async () => {
  let length;
  const runner = async (task, prompt) => {
    if (task.name === "child") length = Buffer.byteLength(prompt);
    return { output: "output" };
  };
  const define = (options) => new Workflow({ runner, ...options }).task("root", "root")
    .task("child", "é", { dependsOn: ["root"] });
  assert.equal((await define({}).run()).ok, true);
  const bound = length;
  assert.equal((await define({ maxInputBytes: bound }).run()).ok, true);
  assert.equal((await define({ maxInputBytes: bound - 1 }).run()).ok, false);
  assert.equal((await new Workflow({ runner, maxInputBytes: 4 }).task("root", "root")
    .task("child", "é", { dependsOn: [{ name: "root", includeOutput: false }] }).run()).ok, true);
});

test("invalid selectors and bounds fail at definition", () => {
  for (const selection of [
    { context: "unknown" }, { context: "summary" }, { summary: "text" },
    { context: "fields" }, { fields: ["a"] }, { context: "fields", fields: ["a", "a"] },
    { context: "artifact", offset: -1 }, { length: 1 }, { context: "artifact", includeOutput: false },
  ]) {
    assert.throws(() => new WorkflowTask("child", "p", { dependsOn: [{ name: "source", ...selection }] }), WorkflowDefinitionError);
  }
  for (const key of ["maxInputBytes", "maxSelectionBytes"]) {
    for (const value of [0, -1, true, 1.5]) {
      assert.throws(() => new Workflow({ [key]: value }), WorkflowDefinitionError);
    }
  }
});

test("native workflow retains reported usage; aggregation does not count dependency reuse", async () => {
  const usage = Object.freeze({ type: "usage", kind: 6, inputTokens: 7n, outputTokens: 2n,
    contextUsed: 10n, contextSize: 100n, cost: 0.25, hasCost: true });
  const original = Runtime.create;
  Runtime.create = async () => ({
    createSession: async () => ({ id: "session", ask: async () => ({ text: "ok", stopReason: "done", usage }),
      close: async () => {} }),
    close: async () => {},
  });
  let result;
  try {
    result = await new Workflow({ runtime: {} }).task("source", "p")
      .task("one", "p", { dependsOn: ["source"] }).task("two", "p", { dependsOn: ["source"] }).run();
  } finally {
    Runtime.create = original;
  }
  assert.deepEqual(result.require("source").usage, usage);
  assert.deepEqual(result.usage, { knownTasks: 3, unknownTasks: 0, inputTokens: 21n, outputTokens: 6n, cost: 0.75 });
  const unknown = await new Workflow({ runner: async (task) => ({ output: "ok",
    usage: task.name === "known" ? { ...usage, cost: undefined, hasCost: false } : undefined,
    error: task.name === "unknown" ? new Error("failed") : undefined,
  }) }).task("known", "p").task("unknown", "p").task("blocked", "p", { dependsOn: ["unknown"] }).run();
  assert.equal(unknown.usage.inputTokens, undefined);
  assert.equal(unknown.usage.cost, undefined);
  assert.equal(unknown.usage.unknownTasks, 1);
  assert.equal(unknown.usage.knownTasks, 1);
  assert.equal(unknown.require("blocked").usage, undefined);
});

test("aborted queued tasks never compose", async () => {
  const original = WorkflowTask.prototype._prompt;
  const rendered = [];
  WorkflowTask.prototype._prompt = function () {
    rendered.push(this.name);
    return original.call(this);
  };
  let enter;
  const entered = new Promise((resolve) => { enter = resolve; });
  const controller = new AbortController();
  const workflow = new Workflow({ maxConcurrency: 1, runner: async (task, prompt, { signal }) => {
    enter();
    await delay(10000, signal);
    return { output: "" };
  }});
  for (let index = 0; index < 32; index++) workflow.task(`task-${index}`, "p");
  const run = workflow.run({ signal: controller.signal });
  try {
    await entered;
    controller.abort();
    await assert.rejects(run);
    assert.deepEqual(rendered, ["task-0"]);
  } finally {
    controller.abort();
    WorkflowTask.prototype._prompt = original;
  }
});

function delay(milliseconds, signal) {
  return new Promise((resolve, reject) => {
    if (signal.aborted) {
      reject(signal.reason);
      return;
    }
    const onAbort = () => {
      clearTimeout(timer);
      reject(signal.reason);
    };
    const timer = setTimeout(() => {
      signal.removeEventListener("abort", onAbort);
      resolve();
    }, milliseconds);
    signal.addEventListener("abort", onAbort, { once: true });
  });
}

function fakeRunner({ delays = {}, failures = new Set(), output = undefined } = {}) {
  const state = {
    active: 0,
    maximum: 0,
    prompts: new Map(),
    started: [],
    finished: [],
  };
  const runner = async (task, prompt, { signal }) => {
    state.active++;
    state.maximum = Math.max(state.maximum, state.active);
    state.started.push(task.name);
    state.prompts.set(task.name, prompt);
    try {
      await delay(delays[task.name] ?? 0, signal);
      if (failures.has(task.name)) throw new Error("fixture task failed");
      return new WorkflowTaskExecution({
        output: output?.(task) ?? `result:${task.name}`,
        sessionId: `session:${task.name}`,
        stopReason: "done",
      });
    } finally {
      state.active--;
      state.finished.push(task.name);
    }
  };
  return { runner, state };
}

test("parallel roots feed deterministic dependency chains", async () => {
  const { runner, state } = fakeRunner({ delays: { research: 40, tests: 40 } });
  const workflow = new Workflow({ maxConcurrency: 2, runner });
  assert.equal(workflow.task("research", "audit the API"), workflow);
  workflow.add("tests", "audit the tests");
  workflow.task("implement", "implement", { dependsOn: ["research", "tests"] });
  workflow.task("ordered", "ordered", {
    dependsOn: [{ name: "implement", includeOutput: false }],
  });

  const result = await workflow.run();

  assert.equal(result.ok, true);
  assert.equal(result.size, 4);
  assert.equal(state.maximum, 2);
  assert.equal(result.output("research"), "result:research");
  assert.equal(result.require("implement").sessionId, "session:implement");
  assert.match(state.prompts.get("implement"), /<dependency name="research">/);
  assert.match(state.prompts.get("implement"), /result:research/);
  assert.match(state.prompts.get("implement"), /result:tests/);
  assert.doesNotMatch(state.prompts.get("ordered"), /tny_workflow_dependencies/);
  assert.ok(state.finished.indexOf("research") < state.started.indexOf("implement"));
  assert.ok(state.finished.indexOf("tests") < state.started.indexOf("implement"));
  assert.deepEqual([...result].map(([name]) => name), [
    "research", "tests", "implement", "ordered",
  ]);
  result.raiseForFailure();
});

test("dependency output inclusion is per edge and preserves declared order", async () => {
  const { runner, state } = fakeRunner();
  const workflow = new Workflow({ runner, maxDependencyBytes: 24 })
    .task("first", "first")
    .task("ordering-only", "ordering-only")
    .task("third", "third")
    .task("consumer", "consume", {
      dependsOn: [
        "first",
        { name: "ordering-only", includeOutput: false },
        { name: "third", includeOutput: true },
      ],
    });

  const result = await workflow.run();

  assert.equal(result.ok, true);
  assert.deepEqual(workflow.tasks.at(-1).dependsOn, [
    { name: "first", includeOutput: true },
    { name: "ordering-only", includeOutput: false },
    { name: "third", includeOutput: true },
  ]);
  assert.equal(Object.isFrozen(workflow.tasks.at(-1).dependsOn), true);
  assert.equal(Object.isFrozen(workflow.tasks.at(-1).dependsOn[0]), true);
  const prompt = state.prompts.get("consumer");
  assert.ok(prompt.indexOf('name="first"') < prompt.indexOf('name="third"'));
  assert.match(prompt, /result:first/);
  assert.doesNotMatch(prompt, /ordering-only/);
  assert.match(prompt, /result:third/);
});

test("a failed branch blocks descendants without cancelling siblings", async () => {
  const { runner, state } = fakeRunner({ failures: new Set(["bad"]) });
  const workflow = new Workflow({ maxConcurrency: 3, runner })
    .task("bad", "fail")
    .task("independent", "succeed")
    .task("child", "blocked", { dependsOn: ["bad"] })
    .task("grandchild", "blocked", { dependsOn: ["child"] });

  const result = await workflow.run();

  assert.equal(result.ok, false);
  assert.equal(result.require("bad").status, WorkflowTaskStatus.failed);
  assert.equal(result.require("independent").status, WorkflowTaskStatus.success);
  assert.equal(result.require("child").status, WorkflowTaskStatus.blocked);
  assert.deepEqual(result.require("child").blockedBy, ["bad"]);
  assert.equal(result.require("grandchild").status, WorkflowTaskStatus.blocked);
  assert.equal(state.started.includes("child"), false);
  assert.throws(
    () => result.raiseForFailure(),
    (error) => error instanceof WorkflowRunError &&
      /bad=failed, child=blocked, grandchild=blocked/.test(error.message),
  );
});

test("missing dependencies and cycles fail before execution", async () => {
  const missing = fakeRunner();
  const missingWorkflow = new Workflow({ runner: missing.runner })
    .task("orphan", "prompt", { dependsOn: ["absent"] });
  await assert.rejects(
    missingWorkflow.run(),
    (error) => error instanceof WorkflowDefinitionError && /undefined task/.test(error.message),
  );
  assert.deepEqual(missing.state.started, []);

  const cycle = fakeRunner();
  const cyclicWorkflow = new Workflow({ runner: cycle.runner })
    .task("first", "prompt", { dependsOn: ["second"] })
    .task("second", "prompt", { dependsOn: ["first"] });
  await assert.rejects(
    cyclicWorkflow.run(),
    (error) => error instanceof WorkflowDefinitionError && /cycle detected/.test(error.message),
  );
  assert.deepEqual(cycle.state.started, []);
});

test("dependency context is bounded before a consumer starts", async () => {
  const fixture = fakeRunner({ output: () => "result-is-too-large" });
  const workflow = new Workflow({
    runner: fixture.runner,
    maxDependencyBytes: 5,
  })
    .task("producer", "produce")
    .task("consumer", "consume", { dependsOn: ["producer"] });

  const result = await workflow.run();

  assert.equal(result.require("producer").status, WorkflowTaskStatus.success);
  assert.equal(result.require("consumer").status, WorkflowTaskStatus.failed);
  assert.ok(result.require("consumer").error instanceof WorkflowContextError);
  assert.equal(fixture.state.started.includes("consumer"), false);
});

test("non-done terminal reasons are task failures with partial output", async () => {
  const workflow = new Workflow({
    runner: async () => ({ output: "partial", stopReason: "denied" }),
  }).task("denied", "prompt");
  const result = await workflow.run();
  assert.equal(result.require("denied").status, WorkflowTaskStatus.failed);
  assert.equal(result.output("denied"), "partial");
  assert.ok(result.require("denied").error instanceof WorkflowRunError);
});

test("AbortSignal cancels active runners and leaves the workflow reusable", async () => {
  const fixture = fakeRunner({ delays: { slow: 200 } });
  const workflow = new Workflow({ runner: fixture.runner }).task("slow", "prompt");
  const controller = new AbortController();
  const reason = new Error("cancel fixture");
  const running = workflow.run({ signal: controller.signal });
  setTimeout(() => controller.abort(reason), 10);
  await assert.rejects(running, (error) => error === reason);
  assert.equal(fixture.state.active, 0);

  fixture.state.finished.length = 0;
  fixture.state.started.length = 0;
  fixture.state.prompts.clear();
  fixture.state.active = 0;
  fixture.state.maximum = 0;
  const second = await workflow.run();
  assert.equal(second.ok, true);
});

test("AbortSignal preserves primitive and cross-realm reasons exactly", async () => {
  const { runInNewContext } = await import("node:vm");
  const reasons = ["cancel fixture", runInNewContext('new Error("cross-realm fixture")')];

  for (const reason of reasons) {
    const fixture = fakeRunner({ delays: { slow: 200 } });
    const workflow = new Workflow({ runner: fixture.runner }).task("slow", "prompt");
    const controller = new AbortController();
    const running = workflow.run({ signal: controller.signal });
    setTimeout(() => controller.abort(reason), 10);
    let rejection;
    try {
      await running;
      assert.fail("aborted workflow unexpectedly resolved");
    } catch (error) {
      rejection = error;
    }
    assert.equal(rejection, reason);
    assert.equal(fixture.state.active, 0);
  }
});

test("definitions and diagnostics do not accidentally render secrets", () => {
  const apiKey = "API-KEY-SECRET";
  const prompt = "PROMPT-SECRET";
  const output = "OUTPUT-SECRET";
  const errorText = "ERROR-SECRET";
  const workflow = new Workflow({
    runtime: {
      workspace: ".",
      apiKey,
      baseUrl: "https://secret.invalid/v1",
    },
  }).task("safe", prompt);
  const unsafeError = new Error("ordinary message");
  unsafeError.name = errorText;
  const execution = new WorkflowTaskExecution({
    output,
    error: unsafeError,
  });
  const rendered = [inspect(workflow), inspect(workflow.tasks[0]), inspect(execution),
    JSON.stringify(workflow), JSON.stringify(workflow.tasks[0]), JSON.stringify(execution)].join(" ");
  for (const secret of [apiKey, prompt, output, errorText, "secret.invalid"]) {
    assert.doesNotMatch(rendered, new RegExp(secret));
  }

  assert.throws(
    () => workflow.task("safe", "duplicate"),
    (error) => error instanceof WorkflowDefinitionError,
  );
  assert.throws(
    () => workflow.task("../escape", "prompt"),
    (error) => error instanceof WorkflowDefinitionError,
  );
  assert.throws(
    () => new Workflow({ maxConcurrency: 0, runner: async () => ({ output: "" }) }),
    (error) => error instanceof WorkflowDefinitionError,
  );
  assert.throws(
    () => new Workflow({ maxDependencyBytes: 1.5, runner: async () => ({ output: "" }) }),
    (error) => error instanceof WorkflowDefinitionError,
  );
  assert.throws(
    () => workflow.task("legacy", "prompt", {
      dependsOn: ["safe"],
      includeDependencies: false,
    }),
    (error) => error instanceof WorkflowDefinitionError && /includeOutput/.test(error.message),
  );
  assert.throws(
    () => workflow.task("invalid-edge", "prompt", {
      dependsOn: [{ name: "safe", includeOutput: "no" }],
    }),
    (error) => error instanceof WorkflowDefinitionError && /includeOutput/.test(error.message),
  );
});
