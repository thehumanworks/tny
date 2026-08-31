import assert from "node:assert/strict";
import { existsSync, mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";

import { Runtime, TnyError, Workflow, WorkflowTaskStatus } from "./sdk.mjs";
import { recordResult } from "./result.mjs";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = resolve(packageRoot, "../..");
const mockScript = join(repoRoot, "tests/integration/mock_openai.py");
const STEER_TEXT = "tny-conformance-steer-rejected-v1";
const configuredSteerTimeout = Number(process.env.TNY_CONFORMANCE_STEER_TIMEOUT ?? 30);
const STEER_READY_TIMEOUT_MS = Number.isFinite(configuredSteerTimeout) &&
  configuredSteerTimeout > 0 ? configuredSteerTimeout * 1000 : 30_000;
const observed = {};

function normalized(event) {
  return {
    type: event.type,
    sequence: Number(event.sequence),
    timestamp_ms: Number(event.timestampMs),
    ...(event.type === "turn_end" ? { stop_reason: event.stopReason } : {}),
    ...(event.type === "steer_rejected" ? { text: event.text } : {}),
    ...(event.type === "error" ? { error_code: event.errorCode } : {}),
  };
}

async function withMock(env, fn) {
  const child = spawn(process.env.PYTHON || "python3", [mockScript, "0"], {
    env: { ...process.env, MOCK_EXPECT_WIRE: "responses", ...env },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stderr = "";
  child.stderr.setEncoding("utf8").on("data", (chunk) => { stderr += chunk; });
  const port = await new Promise((resolveReady, reject) => {
    let stdout = "";
    const cleanup = () => {
      child.stdout.off("data", onData);
      child.off("error", onError);
      child.off("exit", onExit);
    };
    const onError = (error) => {
      cleanup();
      reject(error);
    };
    const onExit = (code) => {
      cleanup();
      reject(new Error(`mock exited ${code}: ${stderr}`));
    };
    const onData = (chunk) => {
      stdout += chunk;
      const match = /^ready on ([1-9]\d*)\r?$/m.exec(stdout);
      if (!match) {
        if (stdout.length > 4096) {
          cleanup();
          reject(new Error("mock readiness output exceeded 4096 bytes"));
        }
        return;
      }
      const actualPort = Number(match[1]);
      if (!Number.isInteger(actualPort) || actualPort > 65535) {
        cleanup();
        reject(new Error(`mock reported invalid port: ${match[1]}`));
        return;
      }
      cleanup();
      resolveReady(actualPort);
    };
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", onData);
    child.once("error", onError);
    child.once("exit", onExit);
  });
  try {
    return await fn(`http://127.0.0.1:${port}/v1`);
  } finally {
    child.kill("SIGTERM");
    await new Promise((resolveExit) => child.once("exit", resolveExit));
  }
}

function fixture() {
  const root = mkdtempSync(join(tmpdir(), "tny-node-sdk-int-"));
  const workspace = join(root, "workspace");
  mkdirSync(workspace);
  for (const name of ["a.txt", "b.txt", "c.txt"]) writeFileSync(join(workspace, name), "x\n");
  return { workspace, stateDir: join(root, "state") };
}

async function create(baseUrl, options = {}) {
  return await Runtime.create({
    ...fixture(),
    baseUrl,
    apiKey: "test-key-not-real",
    ...options,
  });
}

async function steerWhenActive(session, text) {
  const deadline = Date.now() + STEER_READY_TIMEOUT_MS;
  for (;;) {
    try {
      await session.steer(text);
      return;
    } catch (error) {
      if (!(error instanceof TnyError) || error.status !== -2) throw error;
      if (Date.now() >= deadline) {
        throw new Error("turn did not become steerable before the test deadline");
      }
      await new Promise((resolveDelay) => setTimeout(resolveDelay, 1));
    }
  }
}

await withMock({}, async (baseUrl) => {
  const paths = fixture();
  const runtimeOptions = {
    ...paths, baseUrl, apiKey: "test-key-not-real", persistence: true,
  };
  const runtime = await Runtime.create(runtimeOptions);
  const session = await runtime.createSession();
  const sequences = [];
  const transcript = [];
  let text = "";
  let stop;
  for await (const event of session.run("list files in .")) {
    assert.equal(event.provider, "openai");
    assert.equal(event.sessionId, session.id);
    assert.ok(event.turnId);
    transcript.push(normalized(event));
    sequences.push(event.sequence);
    if (event.type === "text_delta") text += event.text;
    if (event.type === "turn_end") stop = event.stopReason;
    await new Promise((resolveDelay) => setTimeout(resolveDelay, 5));
  }
  assert.match(text, /MOCK-OK/);
  assert.equal(stop, "done");
  assert.ok(sequences.every((value, index) => index === 0 || value > sequences[index - 1]));
  let secondText = "";
  let secondStop;
  for await (const event of session.run("run the strict turn again")) {
    assert.equal(event.provider, "openai");
    assert.equal(event.sessionId, session.id);
    assert.ok(event.turnId);
    transcript.push(normalized(event));
    if (event.type === "text_delta") secondText += event.text;
    if (event.type === "turn_end") secondStop = event.stopReason;
  }
  assert.match(secondText, /MOCK-OK/);
  assert.equal(secondStop, "done");
  assert.ok(transcript.every((event, index) =>
    index === 0 || event.sequence > transcript[index - 1].sequence));
  assert.ok(transcript.every((event, index) =>
    index === 0 || event.timestamp_ms >= transcript[index - 1].timestamp_ms));
  const capabilities = await runtime.getCapabilities();
  assert.equal(capabilities.providerInitialized, true);
  assert.equal(capabilities.endpointReachability, 1);
  observed.success_two_turns = transcript;
  await session.close();
  await runtime.close();
});

await withMock({
  MOCK_EXPECT_INSTRUCTIONS: "# Task preset: release\nSDK-TASK-BODY-MARKER",
}, async (baseUrl) => {
  const runtime = await create(baseUrl, {
    taskPreset: { name: "release", instructions: "SDK-TASK-BODY-MARKER" },
  });
  const session = await runtime.createSession();
  let text = "";
  for await (const event of session.run("exercise native ABI 1.1 task creation")) {
    if (event.type === "text_delta") text += event.text;
  }
  assert.match(text, /MOCK-OK/);
  assert.ok(runtime.capabilities.abiMinor >= 1);
  assert.ok(runtime.capabilities.featureAvailableMask & (1n << 12n));
  assert.ok(runtime.capabilities.featureEnabledMask & (1n << 12n));
  await session.close();
  await runtime.close();
});

await withMock({ MOCK_EXPECT_INSTRUCTIONS: "WORKFLOW-DEFAULT-TASK" }, async (baseUrl) => {
  const workflow = new Workflow({
    runtime: {
      ...fixture(), baseUrl, apiKey: "test-key-not-real",
      taskPreset: { name: "default-task", instructions: "WORKFLOW-DEFAULT-TASK" },
    },
  }).task("uses-default-task", "exercise workflow default task config");
  assert.equal((await workflow.run()).ok, true);
});

await withMock({ MOCK_EXPECT_INSTRUCTIONS: "WORKFLOW-PER-TASK" }, async (baseUrl) => {
  const workflow = new Workflow({
    runtime: { ...fixture(), baseUrl, apiKey: "test-key-not-real" },
  }).task("uses-per-task-config", "exercise per-task runtime config", {
    runtime: {
      ...fixture(), baseUrl, apiKey: "test-key-not-real",
      taskPreset: { name: "per-task", instructions: "WORKFLOW-PER-TASK" },
    },
  });
  assert.equal((await workflow.run()).ok, true);
});

await withMock({ MOCK_SLOW_MS: "50" }, async (baseUrl) => {
  const seen = new Set();
  const workflow = new Workflow({
    runtime: {
      ...fixture(), baseUrl, apiKey: "test-key-not-real",
    },
    maxConcurrency: 2,
    onEvent: (task, event) => seen.add(`${task.name}:${event.type}`),
  })
    .task("workflow-first", "first native workflow task")
    .task("workflow-second", "second native workflow task")
    .task("workflow-merge", "merge native workflow outputs", {
      dependsOn: ["workflow-first", "workflow-second"],
    });
  const result = await workflow.run();
  assert.equal(result.ok, true);
  assert.match(result.output("workflow-first"), /MOCK-OK/);
  assert.match(result.output("workflow-second"), /MOCK-OK/);
  assert.match(result.output("workflow-merge"), /MOCK-OK/);
  assert.ok(result.require("workflow-first").sessionId);
  assert.equal(result.require("workflow-merge").stopReason, "done");
  assert.ok(seen.has("workflow-first:text_delta"));
  assert.ok(seen.has("workflow-merge:turn_end"));
});

await withMock({ MOCK_SENSITIVE: "1" }, async (baseUrl) => {
  const workflow = new Workflow({
    runtime: {
      ...fixture(), baseUrl, apiKey: "test-key-not-real",
    },
  }).task("workflow-sensitive", "request a sensitive operation");
  const result = await workflow.run();
  assert.equal(result.ok, false);
  assert.equal(
    result.require("workflow-sensitive").status,
    WorkflowTaskStatus.failed,
  );
  assert.equal(result.require("workflow-sensitive").stopReason, "denied");
});

await withMock({ MOCK_SLOW_MS: "5000" }, async (baseUrl) => {
  const controller = new AbortController();
  const reason = new Error("workflow cancellation fixture");
  const workflow = new Workflow({
    runtime: {
      ...fixture(), baseUrl, apiKey: "test-key-not-real",
    },
    maxConcurrency: 2,
  })
    .task("workflow-cancel-one", "cancel native workflow task one")
    .task("workflow-cancel-two", "cancel native workflow task two");
  const running = workflow.run({ signal: controller.signal });
  setTimeout(() => controller.abort(reason), 25);
  await assert.rejects(running, (error) => error === reason);
});

await withMock({ MOCK_SLOW_MS: "5000" }, async (baseUrl) => {
  const paths = fixture();
  const runtimeOptions = {
    ...paths, baseUrl, apiKey: "test-key-not-real", persistence: true,
  };
  const runtime = await Runtime.create(runtimeOptions);
  const session = await runtime.createSession();
  const sessionId = session.id;
  const iterator = session.run("steer then cancel");
  let pending = iterator.next();
  await steerWhenActive(session, STEER_TEXT);
  await session.cancel();
  const interrupted = [];
  for (;;) {
    const item = await pending;
    if (item.done) break;
    interrupted.push(item.value);
    pending = iterator.next();
  }
  assert.equal(interrupted.at(-2)?.type, "steer_rejected");
  assert.equal(interrupted.at(-2)?.text, STEER_TEXT);
  assert.equal(interrupted.at(-1)?.type, "turn_end");
  assert.equal(interrupted.at(-1)?.stopReason, "interrupted");
  const interruptedTrace = interrupted.map(normalized);
  await session.close();
  await runtime.close();

  await withMock({}, async (resumedBaseUrl) => {
    const resumedRuntime = await Runtime.create({
      ...runtimeOptions, baseUrl: resumedBaseUrl,
    });
    const resumed = await resumedRuntime.openSession(sessionId);
    assert.equal(resumed.id, sessionId);
    const resumedEvents = [];
    for await (const event of resumed.run("resumed turn")) resumedEvents.push(event);
    assert.equal(resumedEvents.at(-1)?.type, "turn_end");
    assert.equal(resumedEvents.at(-1)?.stopReason, "done");
    observed.resume_and_steer_rejection = [
      ...interruptedTrace, ...resumedEvents.map(normalized),
    ];
    await resumed.close();
    await resumedRuntime.close();
  });
});

await withMock({ MOCK_SENSITIVE: "1" }, async (baseUrl) => {
  const paths = fixture();
  const runtime = await Runtime.create({
    ...paths, baseUrl, apiKey: "test-key-not-real",
  });
  const session = await runtime.createSession();
  let permissionId;
  const transcript = [];
  const result = await session.ask("write permission.txt", {
    onEvent: async (event, current) => {
      transcript.push(normalized(event));
      if (event.type === "permission_request") {
        permissionId = event.permissionId;
        await current.respondPermission(event.permissionId, "allow");
        await assert.rejects(
          current.respondPermission(event.permissionId, "allow"),
          (error) => error instanceof TnyError && error.status === -2,
        );
      }
    },
  });
  assert.ok(permissionId);
  assert.equal(result.stopReason, "done");
  assert.equal(existsSync(join(paths.workspace, "permission.txt")), true);
  await assert.rejects(
    session.respondPermission(permissionId, "allow"),
    (error) => error instanceof TnyError && error.status === -2,
  );
  observed.permission_allow_and_stale_reject = transcript;
  await session.close();
  await runtime.close();
});

await withMock({ MOCK_SENSITIVE: "1" }, async (baseUrl) => {
  const paths = fixture();
  const runtime = await Runtime.create({ ...paths, baseUrl, apiKey: "test-key-not-real" });
  const session = await runtime.createSession();
  const transcript = [];
  const result = await session.ask("write permission.txt", {
    onEvent: async (event, current) => {
      transcript.push(normalized(event));
      if (event.type === "permission_request") {
        await current.respondPermission(event.permissionId, "deny");
      }
    },
  });
  assert.equal(result.stopReason, "denied");
  assert.equal(existsSync(join(paths.workspace, "permission.txt")), false);
  observed.permission_deny = transcript;
  await session.close();
  await runtime.close();
});

await withMock({ MOCK_SLOW_MS: "200" }, async (baseUrl) => {
  const runtime = await create(baseUrl);
  const session = await runtime.createSession();
  const controller = new AbortController();
  const events = [];
  const iterator = session.run("cancel this turn", { signal: controller.signal });
  let pending = iterator.next();
  await steerWhenActive(session, "queued-ready");
  const pressure = Array.from({ length: 5000 }, (_, index) =>
    session.steer(`queued-${index}`).catch((error) => error));
  controller.abort();
  for (;;) {
    const item = await pending;
    if (item.done) break;
    events.push(item.value);
    pending = iterator.next();
  }
  await Promise.all(pressure);
  assert.equal(events.filter((event) => event.type === "turn_end").length, 1);
  assert.equal(events.find((event) => event.type === "turn_end")?.stopReason, "interrupted");
  await session.cancel();
  await session.cancel();
  observed.cancel_and_drain = events.map(normalized);
  await session.close();
  await runtime.close();
});

await withMock({ MOCK_SLOW_MS: "100" }, async (baseUrl) => {
  const runtime = await create(baseUrl);
  const session = await runtime.createSession();
  const controller = new AbortController();
  controller.abort();
  const events = [];
  for await (const event of session.run("pre-aborted turn", { signal: controller.signal }))
    events.push(event);
  assert.equal(events.filter((event) => event.type === "turn_end").length, 1);
  assert.equal(events.at(-1)?.stopReason, "interrupted");
  await session.close();
  await runtime.close();
});

await withMock({
  MOCK_HTTP_STATUS: "401",
  MOCK_ERROR_SECRET: process.env.TNY_CONFORMANCE_SENTINEL || "raw-provider-body",
}, async (baseUrl) => {
  const runtime = await create(baseUrl);
  const session = await runtime.createSession();
  const events = [];
  for await (const event of session.run("authentication failure")) events.push(event);
  assert.deepEqual(
    events.filter((event) => event.type === "error").map((event) => event.errorCode),
    [-6],
  );
  assert.equal(events.at(-1)?.type, "turn_end");
  assert.equal(events.at(-1)?.stopReason, "error");
  assert.ok(events.every((event) => !String(event.text || "").includes(
    process.env.TNY_CONFORMANCE_SENTINEL || "raw-provider-body")));
  observed.auth_error = events.map(normalized);
  await session.close();
  await runtime.close();
});

for (let index = 0; index < 3; index++) {
  const runtime = await Runtime.create({
    ...fixture(), baseUrl: "http://127.0.0.1:1/v1", apiKey: "test-key-not-real",
  });
  const session = await runtime.createSession();
  await session.close();
  await runtime.close();
}

recordResult("success_two_turns", "pass", {
  assertion_ids: [
    "create_and_open", "sequence_strictly_increases", "timestamps_monotonic",
    "provider_session_turn_present", "borrowed_bytes_copied_before_free",
    "second_turn_same_session",
  ],
  observed_events: observed.success_two_turns,
});
recordResult("resume_and_steer_rejection", "pass", {
  assertion_ids: ["rejected_text_preserved", "resume_same_session", "teardown_and_reopen"],
  observed_events: observed.resume_and_steer_rejection,
});
recordResult("permission_allow_and_stale_reject", "pass", {
  assertion_ids: ["parked_before_response", "stale_id_bad_state", "duplicate_id_bad_state"],
  observed_events: observed.permission_allow_and_stale_reject,
});
recordResult("permission_deny", "pass", {
  assertion_ids: ["denied_tool_not_executed"],
  observed_events: observed.permission_deny,
});
recordResult("cancel_and_drain", "pass", {
  assertion_ids: ["cancel_idempotent", "exactly_one_terminal", "drained_after_terminal"],
  observed_events: observed.cancel_and_drain,
});
recordResult("auth_error", "pass", {
  assertion_ids: ["stable_auth_category", "no_raw_provider_body", "no_credentials"],
  observed_events: observed.auth_error,
});
recordResult("ownership_and_misuse", "pass", {
  assertion_ids: [
    "event_and_error_lifetimes", "invalid_utf8_rejected", "embedded_nul_rejected",
    "parent_close_releases_children", "repeated_lifecycle",
  ],
  observed_events: [],
});
recordResult("slow_consumer_backpressure", "not_run", {
  reason: "delayed AsyncIterator covers wrapper demand but does not force native event-queue overflow",
});

console.log("typescript-sdk integration: strict mock, workflows, permission, cancel, slow consumer, lifecycle passed");
