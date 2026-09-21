import assert from "node:assert/strict";
import { copyFileSync, mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

import { sdkAddonPath } from "./sdk.mjs";

// Supply an actual earlier ABI-1 library. The subprocess loads the current
// addon against that image in isolation, not another library already in Node.
const legacyLibrary = process.env.TNY_TEST_LEGACY_LIBTNY;

test("current addon loads ABI 1.0-1.3 and preserves native usage defaults", {
  skip: !legacyLibrary && "set TNY_TEST_LEGACY_LIBTNY to an earlier ABI-1 shared library",
  timeout: 30_000,
}, () => {
  const root = mkdtempSync(join(tmpdir(), "tny-node-legacy-abi-"));
  try {
    const addon = join(root, basename(sdkAddonPath));
    copyFileSync(sdkAddonPath, addon);
    const libraryName = process.platform === "darwin" ? "libtny.1.dylib" : "libtny.so.1";
    copyFileSync(legacyLibrary, join(root, libraryName));
    mkdirSync(join(root, "workspace"));
    const script = join(root, "consumer.cjs");
    writeFileSync(script, String.raw`
const assert = require("node:assert/strict");
const http = require("node:http");
const { once } = require("node:events");
const { join } = require("node:path");
const native = require(process.argv[2]);
(async () => {
  const probe = await native.__probeAbi();
  assert.equal(probe.abiVersion >>> 16, 1);
  assert.ok((probe.abiVersion & 0xffff) < 4, "fixture must be older than ABI 1.4");
  const server = http.createServer((request, response) => {
    request.resume();
    request.on("end", () => {
      response.writeHead(200, { "content-type": "text/event-stream" });
      response.write('data: {"id":"legacy","choices":[{"index":0,"delta":{"role":"assistant","content":"legacy-ok"},"finish_reason":null}]}\n\n');
      response.write('data: {"id":"legacy","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":7,"completion_tokens":3}}\n\n');
      response.end('data: [DONE]\n\n');
    });
  });
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  let runtime, session;
  const options = {
    workspace: join(__dirname, "workspace"), stateDir: join(__dirname, "state"),
    provider: "openai", model: "fixture", permissionMode: 2, persistence: false,
    apiKey: "synthetic-legacy-fixture", wireApi: "chat",
    baseUrl: "http://127.0.0.1:" + server.address().port + "/v1",
  };
  try {
    await assert.rejects(native.createRuntime({ ...options, acpCommandJson: '["unused"]' }),
      /acpCommand requires libtny ABI 1\.4/);
    runtime = await native.createRuntime(options);
    assert.equal(runtime.abiVersion, probe.abiVersion);
    session = await native.createSession(runtime.runtimeId);
    await native.send(runtime.runtimeId, session.sessionHandle, "focused legacy ABI usage check");
    const events = [];
    for (;;) {
      const item = await native.nextEvent(runtime.runtimeId, session.sessionHandle);
      if (item.done) break;
      events.push(item.value);
      assert.ok(events.length < 100, "bounded native event stream");
    }
    assert.ok(events.some(event => event.type === "turn_end" && event.stopReason === "done"));
    const usage = events.filter(event => event.type === "usage").at(-1);
    assert.ok(usage, "actual native usage event must reach the current addon");
    assert.equal(usage.inputTokens, 7n);
    assert.equal(usage.outputTokens, 3n);
    assert.equal(usage.costCurrency, "");
    assert.equal(usage.costCumulative, false);
    assert.equal(usage.tokensReported, true);
    console.log("legacy ABI " + (probe.abiVersion & 0xffff) + " native usage and ACP rejection passed");
  } finally {
    if (session) await native.closeSession(runtime.runtimeId, session.sessionHandle);
    if (runtime) await native.closeRuntime(runtime.runtimeId);
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
`);
    const run = spawnSync(process.execPath, [script, addon], {
      encoding: "utf8", timeout: 25_000,
      env: { ...process.env, DYLD_LIBRARY_PATH: root, LD_LIBRARY_PATH: root },
    });
    assert.equal(run.error, undefined);
    assert.equal(run.status, 0, run.stderr + run.stdout);
    assert.match(run.stdout, /native usage and ACP rejection passed/);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
