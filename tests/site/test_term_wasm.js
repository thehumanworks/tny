#!/usr/bin/env node
"use strict";

// Run the actual bootstrap with a terminal double and an in-memory wasm module.
// No browser, provider requests, or compiled wasm dependency is needed here.
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const { test } = require("node:test");
const C = require(path.resolve(__dirname, "../../site/assets/term-core.js"));
const source = fs.readFileSync(
  process.argv[2] || path.resolve(__dirname, "../../site/assets/term-wasm.js"), "utf8"
);
const settle = () => new Promise(setImmediate);

async function launch(boot = {}) {
  let onData;
  let output = "";
  let config;
  let starts = 0;
  const mount = { clientWidth: 640, clientHeight: 384, setAttribute() {} };
  class Terminal {
    constructor(options) {
      this.options = options;
      this.cols = 80;
      this.rows = 24;
      this._core = { _renderService: { dimensions: { css: { cell: { width: 8, height: 16 } } } } };
    }
    open() {}
    focus() {}
    onResize() {}
    onData(fn) { onData = fn; }
    resize(cols, rows) { this.cols = cols; this.rows = rows; }
    write(text) { output += text; }
  }
  const context = vm.createContext({
    window: { tnyTermCore: C, __tnyBoot: boot, Terminal, addEventListener() {} },
    document: { querySelector() { return mount; } },
    TextEncoder,
    setTimeout,
  });
  const module = new vm.SyntheticModule(["default"], function () {
    this.setExport("default", async (options) => {
      config = options;
      return { callMain() { starts++; } };
    });
  }, { context });
  new vm.Script(source, {
    filename: "term-wasm.js",
    importModuleDynamically: async (specifier) => {
      assert.strictEqual(specifier, "./wasm/tny-web.mjs");
      await module.link(() => {});
      await module.evaluate();
      return module;
    },
  }).runInContext(context);
  await settle();
  return {
    get env() { return config && config.tnyEnv; },
    get output() { return output; },
    get starts() { return starts; },
    async type(text) { onData(text); await settle(); },
  };
}

test("URL credentials and model reach the real module boundary", async () => {
  const app = await launch({
    apiKey: "\u00a0sk-test\u200b", baseUrl: "https://gateway.example/v1/", model: " model-x ",
  });
  assert.strictEqual(app.starts, 1);
  assert.strictEqual(app.env.OPENAI_API_KEY, "sk-test");
  assert.strictEqual(app.env.OPENAI_BASE_URL, "https://gateway.example/v1");
  assert.strictEqual(app.env.OPENAI_MODEL, "model-x");
  assert.ok(!app.output.includes("sk-test"));
});

test("named provider settings remain together", async () => {
  const app = await launch({ env: {
    GROQ_API_KEY: "\u200bsk-named", GROQ_BASE_URL: "https://gateway.example/v1/",
    GROQ_DEFAULT_MODEL: "model-y", GROQ_WIRE_API: "chat",
  } });
  assert.strictEqual(app.starts, 1);
  assert.strictEqual(app.env.GROQ_API_KEY, "sk-named");
  assert.strictEqual(app.env.GROQ_BASE_URL, "https://gateway.example/v1");
  assert.strictEqual(app.env.GROQ_DEFAULT_MODEL, "model-y");
  assert.strictEqual(app.env.GROQ_WIRE_API, "chat");
  assert.strictEqual(app.env.OPENAI_API_KEY, undefined);
});

for (const [name, boot, error] of [
  ["invalid URL key", { apiKey: "sk-private\u2026" }, /U\+2026/],
  ["invalid URL base", { apiKey: "sk-private", baseUrl: "file:///tmp/api" }, /http\(s\)/],
  ["invalid named base", { env: { GROQ_API_KEY: "sk-private", GROQ_BASE_URL: "bad" } }, /GROQ_BASE_URL/],
  ["invalid named key", { env: { GROQ_API_KEY: "sk-private\u2026" } }, /GROQ_API_KEY/],
  ["empty named key", { env: { GROQ_API_KEY: "\u200b" } }, /GROQ_API_KEY/],
]) {
  test(name + " reports an error without launching or exposing the key", async () => {
    const app = await launch(boot);
    assert.strictEqual(app.starts, 0);
    assert.strictEqual(app.env, undefined);
    assert.match(app.output, error);
    assert.match(app.output, /Reload the page/);
    assert.ok(!app.output.includes("sk-private"));
  });
}

test("invalid pasted keys retry the prompt without exposing key material", async () => {
  const app = await launch();
  await app.type("sk-private\u2026\r");
  assert.match(app.output, /U\+2026/);
  assert.strictEqual(app.starts, 0);
  await app.type("\u200b\r");
  assert.match(app.output, /empty after removing/);
  await app.type("sk-valid\r");
  await app.type("\r");
  assert.strictEqual(app.starts, 1);
  assert.strictEqual(app.env.OPENAI_API_KEY, "sk-valid");
  assert.strictEqual(app.env.OPENAI_BASE_URL, C.DEFAULT_BASE);
  assert.ok(!app.output.includes("sk-private"));
  assert.ok(!app.output.includes("sk-valid"));
});

test("invalid prompted bases retry before any module launch", async () => {
  const app = await launch();
  await app.type("sk-valid\r");
  for (const base of ["javascript:alert(1)", "relative/path"]) {
    await app.type(base + "\r");
    assert.strictEqual(app.starts, 0);
    assert.strictEqual(app.env, undefined);
  }
  await app.type("http://127.0.0.1:8080/v1/\r");
  assert.strictEqual(app.starts, 1);
  assert.strictEqual(app.env.OPENAI_BASE_URL, "http://127.0.0.1:8080/v1");
});

test("empty key keeps keyless startup and the selected model", async () => {
  const app = await launch({ model: "local-model", baseUrl: "http://127.0.0.1:8080/v1" });
  await app.type("\r");
  assert.strictEqual(app.starts, 1);
  assert.strictEqual(app.env.OPENAI_API_KEY, undefined);
  assert.strictEqual(app.env.OPENAI_MODEL, "local-model");
  assert.strictEqual(app.env.OPENAI_BASE_URL, "http://127.0.0.1:8080/v1");
});

test("blank base input uses the validated URL setting", async () => {
  const app = await launch({ baseUrl: "https://gateway.example/v1/" });
  await app.type("sk-valid\r");
  await app.type("\r");
  assert.strictEqual(app.starts, 1);
  assert.strictEqual(app.env.OPENAI_BASE_URL, "https://gateway.example/v1");
});
