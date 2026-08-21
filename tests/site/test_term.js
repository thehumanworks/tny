#!/usr/bin/env node
"use strict";

const path = require("path");
const assert = require("assert");
const { webcrypto } = require("crypto");

if (!global.crypto) global.crypto = webcrypto;
if (typeof global.btoa !== "function") {
  global.btoa = (s) => Buffer.from(s, "binary").toString("base64");
  global.atob = (s) => Buffer.from(s, "base64").toString("binary");
}

const C = require(path.resolve(__dirname, "../../site/assets/term-core.js"));

let failed = 0;
function test(name, fn) {
  const p = Promise.resolve().then(fn);
  return p.then(
    () => console.log("  ok  " + name),
    (err) => {
      failed++;
      console.error("  FAIL " + name);
      console.error("    " + (err && err.stack ? err.stack : err));
    }
  );
}

async function run() {
  console.log("== site term-core");

  await test("parse hash and query secrets", () => {
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

  await test("short aliases key/base/model", () => {
    const got = C.parseParamString("key=sk-a&base=https://api.example/v1&model=m");
    assert.strictEqual(got.apiKey, "sk-a");
    assert.strictEqual(got.baseUrl, "https://api.example/v1");
    assert.strictEqual(got.model, "m");
  });

  await test("takeSecretsFromLocation strips secrets and keeps other params", () => {
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

  await test("parse env assignments and refuse to treat them as chat", () => {
    const got = C.parseEnvAssignments(
      'export OPENAI_API_KEY="sk-quoted" OPENAI_BASE_URL=https://corp.internal/v1'
    );
    assert.strictEqual(got.apiKey, "sk-quoted");
    assert.strictEqual(got.baseUrl, "https://corp.internal/v1");
    assert.ok(C.looksLikeSecretAssignment("OPENAI_API_KEY=sk-x"));
    assert.ok(!C.looksLikeSecretAssignment("what is OPENAI_API_KEY used for?"));
  });

  await test("sanitizeBaseUrl rejects non-http schemes", () => {
    assert.strictEqual(C.sanitizeBaseUrl(""), C.DEFAULT_BASE);
    assert.strictEqual(C.sanitizeBaseUrl("https://api.openai.com/v1/"), "https://api.openai.com/v1");
    assert.throws(() => C.sanitizeBaseUrl("javascript:alert(1)"), /http/);
    assert.throws(() => C.sanitizeBaseUrl("not a url"), /absolute/);
  });

  await test("joinApi and obfuscateUrl never echo the host", () => {
    const url = "https://private-gateway.internal.example/v1";
    const joined = C.joinApi(url, "chat/completions");
    assert.strictEqual(joined, url + "/chat/completions");
    const shown = C.obfuscateUrl(url);
    assert.ok(!shown.includes("private-gateway"));
    assert.ok(!shown.includes("internal.example"));
    assert.ok(shown.startsWith("https://"));
    assert.ok(shown.includes("/***") || shown.endsWith("/***"));
  });

  await test("redactText replaces key and base url", () => {
    const creds = {
      apiKey: "sk-super-secret-value",
      baseUrl: "https://hidden.example/v1",
    };
    const out = C.redactText(
      "using sk-super-secret-value at https://hidden.example/v1",
      creds
    );
    assert.ok(!out.includes("sk-super-secret-value"));
    assert.ok(!out.includes("hidden.example"));
    assert.ok(out.includes("•"));
  });

  await test("maskSecret hides the middle", () => {
    const m = C.maskSecret("sk-abcdefghijk");
    assert.ok(m.startsWith("sk-"));
    assert.ok(m.endsWith("ijk"));
    assert.ok(m.includes("•"));
    assert.ok(!m.includes("abcdef"));
  });

  await test("SseParser survives every split boundary", () => {
    const frames = [
      'data: {"choices":[{"delta":{"content":"Hel"}}]}\n\n',
      'data: {"choices":[{"delta":{"content":"lo"}}]}\n\n',
      "data: [DONE]\n\n",
    ];
    const stream = frames.join("");
    for (let split = 0; split <= stream.length; split++) {
      const p = new C.SseParser();
      const a = p.push(stream.slice(0, split));
      const b = p.push(stream.slice(split));
      const events = a.concat(b);
      const texts = events
        .filter((e) => e.json)
        .map((e) => e.json.choices[0].delta.content)
        .join("");
      const dones = events.filter((e) => e.done).length;
      assert.strictEqual(texts, "Hello", "split " + split);
      assert.strictEqual(dones, 1, "done at split " + split);
    }
  });

  await test("AES-GCM roundtrip", async () => {
    const key = await C.aesGcmGenerateKey();
    const pt = new TextEncoder().encode(JSON.stringify({ k: "sk-round", u: "https://z.example/v1" }));
    const sealed = await C.aesGcmSeal(key, pt);
    assert.strictEqual(sealed.iv.length, 12);
    assert.ok(sealed.ct.length > 0);
    const opened = await C.aesGcmOpen(key, sealed.iv, sealed.ct);
    assert.strictEqual(new TextDecoder().decode(opened), new TextDecoder().decode(pt));
    const other = await C.aesGcmGenerateKey();
    let threw = false;
    try {
      await C.aesGcmOpen(other, sealed.iv, sealed.ct);
    } catch (e) {
      threw = true;
    }
    assert.ok(threw, "wrong key must fail");
  });

  if (failed) {
    console.error(failed + " failed");
    process.exit(1);
  }
  console.log("   ok");
}

run();
