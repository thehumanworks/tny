import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import {
  PermissionDecision,
  Runtime,
  TnyError,
  UnsupportedFeatureError,
  eventKinds,
} from "./sdk.mjs";
import { sdkPackageRoot } from "./sdk.mjs";
import { recordResult } from "./result.mjs";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const require = createRequire(import.meta.url);
const native = require(join(sdkPackageRoot, "build/Release/tny.node"));

function paths() {
  const root = mkdtempSync(join(tmpdir(), "tny-node-sdk-"));
  const workspace = join(root, "workspace");
  mkdirSync(workspace);
  return { workspace, stateDir: join(root, "state") };
}

test("exports canonical event ids", () => {
  assert.equal(eventKinds.text_delta, 0);
  assert.equal(eventKinds.tool_progress, 13);
  assert.equal(PermissionDecision.deny, 2);
});

test("environment cleanup uses no-allocation signalled shutdown", () => {
  const owner = readFileSync(join(packageRoot, "native/owner.c"), "utf8");
  const cleanup = owner.slice(owner.indexOf("void sdk_cleanup_env"));
  assert.doesNotMatch(cleanup, /\b(?:malloc|calloc|realloc)\s*\(/);
  assert.match(cleanup, /atomic_store/);
  assert.match(cleanup, /pthread_cond_signal/);
});

test("known and unknown native events match the canonical schema", () => {
  const schema = JSON.parse(readFileSync(resolve(packageRoot, "../schema/events.json"), "utf8"));
  assert.deepEqual(eventKinds, Object.fromEntries(schema.events.map((event) => [event.type, event.id])));
  const declarations = readFileSync(join(packageRoot, "dist/index.d.ts"), "utf8");
  for (const event of schema.events) {
    const decoded = native.__testDecodeEvent(event.id);
    assert.equal(decoded.type, event.type);
    assert.equal(decoded.kind, event.id);
    assert.match(declarations, new RegExp(`type: "${event.type}"; kind: ${event.id};`));
  }
  const unknown = native.__testDecodeEvent(99);
  assert.equal(unknown.type, "unknown");
  assert.equal(unknown.kind, 99);
  assert.equal(unknown.payload.text.length, 3);
  assert.deepEqual([...unknown.payload.text].map((value) => value.charCodeAt(0)), [65, 0, 66]);
  assert.equal(unknown.payload.messageType, "future-type");
  assert.equal(unknown.payload.errorCode, -123);
  const nulKnown = native.__testDecodeEvent(0);
  assert.equal(nulKnown.text.length, 3);
  assert.deepEqual([...nulKnown.text].map((value) => value.charCodeAt(0)), [65, 0, 66]);
  recordResult("unknown_future_event", "pass", {
    assertion_ids: ["numeric_kind_preserved", "payload_preserved", "known_union_not_aliased"],
    observed_events: [{
      type: unknown.type,
      sequence: Number(unknown.sequence),
      timestamp_ms: Number(unknown.timestampMs),
      kind: unknown.kind,
      payload: {
        text: unknown.payload.text,
        messageType: unknown.payload.messageType,
        errorCode: unknown.payload.errorCode,
      },
    }],
  });
});

test("rejects unsupported provider and send_ex features precisely", async () => {
  await assert.rejects(
    Runtime.create({ ...paths(), provider: "cursor" }),
    (error) => error instanceof UnsupportedFeatureError && error.status === -9,
  );
  const runtime = await Runtime.create({
    ...paths(), baseUrl: "http://127.0.0.1:1/v1", apiKey: "test-key-not-real",
  });
  const session = await runtime.createSession();
  await assert.rejects(
    async () => {
      for await (const _ of session.run("hello", { images: [{}] })) {}
    },
    (error) => error instanceof UnsupportedFeatureError && error.feature === "images",
  );
  await session.close();
  await runtime.close();
});

test("explicit close is idempotent and closed handles are rejected", async () => {
  const runtime = await Runtime.create({
    ...paths(), baseUrl: "http://127.0.0.1:1/v1", apiKey: "test-key-not-real",
  });
  assert.equal(runtime.capabilities.abiMajor, 1);
  assert.ok(runtime.capabilities.abiMinor >= 0);
  assert.equal(runtime.capabilities.providerSelected, 1);
  assert.equal(runtime.capabilities.threadingModel, 1);
  assert.equal((runtime.capabilities.featureEnabledMask & 128n), 128n);
  assert.deepEqual(await runtime.getCapabilities(), runtime.capabilities);
  const session = await runtime.createSession();
  await session.close();
  await session.close();
  await assert.rejects(session.cancel(), (error) => error instanceof TnyError && error.status === -2);
  await runtime.close();
  await runtime.close();
});

test("concurrent native operations and runtime close retain owner state", async () => {
  for (let iteration = 0; iteration < 10; iteration++) {
    const runtime = await Runtime.create({
      ...paths(), baseUrl: "http://127.0.0.1:1/v1", apiKey: "test-key-not-real",
    });
    const session = await runtime.createSession();
    const operations = Array.from({ length: 20 }, () => session.cancel());
    const closing = runtime.close();
    const results = await Promise.allSettled([...operations, closing]);
    assert.equal(results.at(-1)?.status, "fulfilled");
    assert.equal(runtime.closed, true);
  }
});

test("ephemeral runtime may omit stateDir", async () => {
  const { workspace } = paths();
  const runtime = await Runtime.create({
    workspace, baseUrl: "http://127.0.0.1:1/v1", apiKey: "test-key-not-real",
  });
  const session = await runtime.createSession();
  assert.ok(session.id);
  await session.close();
  await runtime.close();
});

test("simultaneous runtimes remain isolated", async () => {
  const options = () => ({
    ...paths(), baseUrl: "http://127.0.0.1:1/v1", apiKey: "test-key-not-real",
  });
  const [first, second] = await Promise.all([Runtime.create(options()), Runtime.create(options())]);
  const [firstSession, secondSession] = await Promise.all([
    first.createSession(), second.createSession(),
  ]);
  assert.notEqual(firstSession.id, secondSession.id);
  await first.close();
  assert.equal((await second.getCapabilities()).abiMajor, 1);
  await secondSession.close();
  await second.close();
});
