import assert from "node:assert/strict";
import { inspect } from "node:util";
import test from "node:test";

import {
  Workflow,
  WorkflowContextError,
  WorkflowDefinitionError,
  WorkflowRunError,
  WorkflowTaskExecution,
  WorkflowTaskStatus,
} from "./sdk.mjs";

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
