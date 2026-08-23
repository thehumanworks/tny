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

  await test("named provider env pairs ride the hash and are stripped", () => {
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

  await test("parse env assignments and refuse to treat them as chat", () => {
    const got = C.parseEnvAssignments(
      'export OPENAI_API_KEY="sk-quoted" OPENAI_BASE_URL=https://corp.internal/v1'
    );
    assert.strictEqual(got.apiKey, "sk-quoted");
    assert.strictEqual(got.baseUrl, "https://corp.internal/v1");
    assert.ok(C.looksLikeSecretAssignment("OPENAI_API_KEY=sk-x"));
    assert.ok(!C.looksLikeSecretAssignment("what is OPENAI_API_KEY used for?"));
    assert.ok(C.looksLikeSecretDraft("OPENAI_API_KEY="));
    assert.ok(C.looksLikeSecretDraft("/login sk-x"));
    assert.ok(C.looksLikeSecretDraft("/setup OPENAI_BASE_URL=https://x.example/v1"));
    assert.ok(!C.looksLikeSecretDraft("/help"));
    assert.ok(!C.looksLikeSecretDraft("explain OPENAI_API_KEY"));
  });

  await test("sanitizeBaseUrl rejects non-http schemes", () => {
    assert.strictEqual(C.sanitizeBaseUrl(""), C.DEFAULT_BASE);
    assert.strictEqual(C.sanitizeBaseUrl("https://api.openai.com/v1/"), "https://api.openai.com/v1");
    assert.throws(() => C.sanitizeBaseUrl("javascript:alert(1)"), /http/);
    assert.throws(() => C.sanitizeBaseUrl("not a url"), /absolute/);
  });

  await test("sanitizeApiKey strips pasted junk and rejects non-ASCII", () => {
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

  await test("toResponsesInput translates chat-shaped history", () => {
    const items = C.toResponsesInput([
      { role: "user", content: "hello" },
      { role: "assistant", content: "hi" },
      {
        role: "assistant",
        content: null,
        tool_calls: [
          { id: "call_9", type: "function", function: { name: "lookup_docs", arguments: '{"topic":"cli"}' } },
        ],
      },
      { role: "tool", tool_call_id: "call_9", content: "docs text" },
      { role: "bogus-no-content" },
      null,
    ]);
    assert.deepStrictEqual(items, [
      { role: "user", content: "hello" },
      { role: "assistant", content: "hi" },
      { type: "function_call", call_id: "call_9", name: "lookup_docs", arguments: '{"topic":"cli"}' },
      { type: "function_call_output", call_id: "call_9", output: "docs text" },
      { role: "bogus-no-content", content: "" },
    ]);
  });

  await test("ResponsesTurn assembles typed events across every split boundary", () => {
    const frames = [
      'data: {"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"lookup_docs","arguments":""}}\n\n',
      'data: {"type":"response.function_call_arguments.delta","output_index":0,"delta":"{\\"top"}\n\n',
      'data: {"type":"response.function_call_arguments.delta","output_index":0,"delta":"ic\\":\\"cli\\"}"}\n\n',
      // done with EMPTY arguments must not wipe the assembled deltas
      'data: {"type":"response.output_item.done","output_index":0,"item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"lookup_docs","arguments":""}}\n\n',
      'data: {"type":"response.output_text.delta","output_index":1,"delta":"Hel"}\n\n',
      'data: {"type":"response.output_text.delta","output_index":1,"delta":"lo"}\n\n',
      'data: {"type":"response.completed","response":{"status":"completed"}}\n\n',
    ];
    const stream = frames.join("");
    for (let split = 0; split <= stream.length; split += 7) {
      const p = new C.SseParser();
      const turn = new C.ResponsesTurn();
      let painted = "";
      p.push(stream.slice(0, split))
        .concat(p.push(stream.slice(split)))
        .forEach((ev) => {
          if (ev.json) painted += turn.push(ev.json);
        });
      assert.strictEqual(turn.content, "Hello", "split " + split);
      assert.strictEqual(painted, "Hello", "painted at split " + split);
      assert.ok(turn.completed, "completed at split " + split);
      assert.strictEqual(turn.error, null);
      assert.strictEqual(turn.calls.length, 1);
      assert.strictEqual(turn.calls[0].id, "call_1");
      assert.strictEqual(turn.calls[0].function.name, "lookup_docs");
      assert.strictEqual(turn.calls[0].function.arguments, '{"topic":"cli"}');
    }
  });

  await test("ResponsesTurn: late metadata, authoritative done, junk events", () => {
    const turn = new C.ResponsesTurn();
    // added announces only the item; call_id/name/arguments arrive in done
    turn.push({ type: "response.output_item.added", output_index: 0, item: { type: "function_call", id: "fc_2" } });
    turn.push({ type: "response.output_item.added", output_index: 3, item: { id: "no-type" } });
    turn.push({ type: "response.output_text.delta" }); // no delta member
    turn.push({ type: "response.function_call_arguments.delta", output_index: 9, delta: "{}" }); // unknown index
    turn.push({
      type: "response.output_item.done",
      output_index: 0,
      item: { type: "function_call", id: "fc_2", call_id: "call_2", name: "lookup_docs", arguments: '{"topic":"size"}' },
    });
    turn.push({ type: "response.completed", response: { status: "completed" } });
    assert.strictEqual(turn.calls.length, 1);
    assert.strictEqual(turn.calls[0].id, "call_2");
    assert.strictEqual(turn.calls[0].function.arguments, '{"topic":"size"}');
    assert.ok(turn.completed);
  });

  await test("ResponsesTurn surfaces response.failed and error events", () => {
    const t1 = new C.ResponsesTurn();
    t1.push({ type: "response.failed", response: { error: { message: "mock exploded" } } });
    assert.strictEqual(t1.error, "mock exploded");
    assert.ok(t1.completed);
    const t2 = new C.ResponsesTurn();
    t2.push({ type: "error", message: "top-level error" });
    assert.strictEqual(t2.error, "top-level error");
    // incomplete keeps the partial text and no error
    const t3 = new C.ResponsesTurn();
    t3.push({ type: "response.output_text.delta", delta: "partial" });
    t3.push({ type: "response.incomplete", response: { status: "incomplete" } });
    assert.strictEqual(t3.content, "partial");
    assert.strictEqual(t3.error, null);
    assert.ok(t3.completed);
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
