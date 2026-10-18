#!/usr/bin/env node
"use strict";

const path = require("path");
const assert = require("assert");
const C = require(path.resolve(__dirname, "../../site/assets/term-core.js"));

let failed = 0;
function test(name, fn) {
  try {
    fn();
    console.log("  ok  " + name);
  } catch (err) {
    failed++;
    console.error("  FAIL " + name);
    console.error("    " + (err && err.stack ? err.stack : err));
  }
}

function run() {
  console.log("== site term-core");

  test("parse hash and query secrets", () => {
    const q = C.parseParamString("?x=1&OPENAI_API_KEY=sk-test&OPENAI_MODEL=gpt-x");
    assert.strictEqual(q.apiKey, "sk-test");
    assert.strictEqual(q.model, "gpt-x");
    assert.strictEqual(q.baseUrl, undefined);
    const h = C.parseParamString(
      "#OPENAI_API_KEY=sk-h&OPENAI_BASE_URL=" + encodeURIComponent("https://gateway.example/v1")
    );
    assert.strictEqual(h.apiKey, "sk-h");
    assert.strictEqual(h.baseUrl, "https://gateway.example/v1");
  });

  test("short aliases key/base/model", () => {
    const got = C.parseParamString("key=sk-a&base=https://api.example/v1&model=m");
    assert.strictEqual(got.apiKey, "sk-a");
    assert.strictEqual(got.baseUrl, "https://api.example/v1");
    assert.strictEqual(got.model, "m");
  });

  test("takeSecretsFromLocation strips secrets and keeps other params", () => {
    const loc = {
      pathname: "/tny/",
      search: "?OPENAI_API_KEY=sk-leak&keep=yes",
      hash: "#OPENAI_BASE_URL=" + encodeURIComponent("https://secret.example/v1") + "&left=1",
    };
    const replaced = [];
    const hist = { replaceState(_s, _t, url) { replaced.push(url); } };
    const got = C.takeSecretsFromLocation(loc, hist);
    assert.strictEqual(got.apiKey, "sk-leak");
    assert.strictEqual(got.baseUrl, "https://secret.example/v1");
    assert.strictEqual(replaced.length, 1);
    assert.strictEqual(replaced[0], "/tny/?keep=yes#left=1");
    assert.ok(!replaced[0].includes("sk-leak"));
    assert.ok(!replaced[0].includes("secret.example"));
  });

  test("named provider env pairs ride the hash and are stripped", () => {
    const loc = {
      pathname: "/tny/",
      search: "",
      hash: "#OPENROUTER_API_KEY=sk-named&OPENROUTER_BASE_URL=" +
            encodeURIComponent("https://openrouter.ai/api/v1") +
            "&GROQ_DEFAULT_MODEL=llama-4&keep=1",
    };
    const replaced = [];
    const hist = { replaceState(_s, _t, url) { replaced.push(url); } };
    const got = C.takeSecretsFromLocation(loc, hist);
    assert.strictEqual(got.env.OPENROUTER_API_KEY, "sk-named");
    assert.strictEqual(got.env.OPENROUTER_BASE_URL, "https://openrouter.ai/api/v1");
    assert.strictEqual(got.env.GROQ_DEFAULT_MODEL, "llama-4");
    assert.strictEqual(replaced.length, 1);
    assert.strictEqual(replaced[0], "/tny/#keep=1");
    assert.ok(!replaced[0].includes("sk-named"));
    assert.ok(!replaced[0].includes("openrouter.ai"));
    // lowercase and unrelated params never register as env pairs
    const none = C.takeSecretsFromLocation(
      { pathname: "/", search: "", hash: "#foo_api_key=x&BASE_URL=y" },
      { replaceState() { throw new Error("nothing to strip"); } });
    assert.ok(!none.env);
  });

  test("sanitizeBaseUrl rejects non-http schemes", () => {
    assert.strictEqual(C.sanitizeBaseUrl(""), C.DEFAULT_BASE);
    assert.strictEqual(C.sanitizeBaseUrl("https://api.openai.com/v1/"), "https://api.openai.com/v1");
    assert.throws(() => C.sanitizeBaseUrl("javascript:alert(1)"), /http/);
    assert.throws(() => C.sanitizeBaseUrl("not a url"), /absolute/);
  });

  test("sanitizeApiKey strips pasted junk and rejects non-ASCII", () => {
    assert.strictEqual(C.sanitizeApiKey("sk-plain"), "sk-plain");
    assert.strictEqual(C.sanitizeApiKey("  sk-trimmed\n"), "sk-trimmed");
    // NBSP, zero-width space, word joiner, BOM, bidi marks: pasted from rich text
    assert.strictEqual(
      C.sanitizeApiKey("\u00a0sk-\u200bab\u2060cd\ufeff\u200e"),
      "sk-abcd"
    );
    assert.strictEqual(C.sanitizeApiKey(null), "");
    assert.strictEqual(C.sanitizeApiKey("\u200b\ufeff"), "");
    // Ellipsis (from copying the /setup placeholder), curly quotes, emoji
    assert.throws(() => C.sanitizeApiKey("sk-abc\u2026"), /U\+2026/);
    assert.throws(() => C.sanitizeApiKey("\u201csk-quoted\u201d"), /HTTP header/);
    assert.throws(() => C.sanitizeApiKey("sk-\u00e9"), /U\+00E9/);
    // Error must not echo the key material itself
    try {
      C.sanitizeApiKey("sk-secret\u2026value");
    } catch (e) {
      assert.ok(!String(e.message).includes("sk-secret"));
    }
  });

  test("wrapToCols word-wraps and keeps newlines", () => {
    assert.deepStrictEqual(C.wrapToCols("hello world", 5), ["hello", "world"]);
    assert.deepStrictEqual(C.wrapToCols("abcdefghij", 4), ["abcd", "efgh", "ij"]);
    assert.deepStrictEqual(C.wrapToCols("a\nb c d", 3), ["a", "b c", "d"]);
    assert.deepStrictEqual(C.wrapToCols("", 20), [""]);
    assert.deepStrictEqual(C.wrapToCols("ab", 0), ["a", "b"]);
    assert.ok(C.wrapToCols("Bring an OpenAI-compatible key;", 28).every((l) => l.length <= 28));
  });

  test("proposeTermGeometry never invents columns that do not fit", () => {
    assert.deepStrictEqual(C.proposeTermGeometry(390, 300, 7.5, 15), { cols: 52, rows: 20 });
    assert.deepStrictEqual(C.proposeTermGeometry(0, 0, 8, 16), { cols: 2, rows: 1 });
    assert.deepStrictEqual(C.proposeTermGeometry(100, 80, 8, 16), { cols: 12, rows: 5 });
    assert.strictEqual(C.proposeTermGeometry(320, 200, 8, 16).cols < 80, true);
  });

  if (failed) {
    console.error(failed + " failed");
    process.exit(1);
  }
  console.log("   ok");
}

run();
