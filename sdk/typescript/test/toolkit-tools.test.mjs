// Toolkit.listTools / callTool: embedded native tools (ADR 0223).
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { Toolkit, TnyError } from "./sdk.mjs";

function setup(t, settings = {}) {
  const workspace = realpathSync(mkdtempSync(join(tmpdir(), "tny-node-tools-")));
  t.after(() => rmSync(workspace, { recursive: true, force: true }));
  writeFileSync(join(workspace, "settings.json"), JSON.stringify(settings));
  return { workspace, toolkit: new Toolkit({ workspace, settingsPath: "settings.json" }) };
}

test("listTools returns the embedded nested catalog", async t => {
  const { toolkit } = setup(t);
  const tools = await toolkit.listTools();
  assert.ok(Object.isFrozen(tools));
  const names = tools.map(tool => tool.name);
  for (const name of ["read_file", "write_file", "grep_files", "web_fetch"]) assert.ok(names.includes(name), name);
  for (const name of ["read_image", "ask_user_question", "terminal", "subagent"]) assert.ok(!names.includes(name), name);
  const read = tools.find(tool => tool.name === "read_file");
  assert.equal(read.parameters.type, "object");
  assert.ok(read.description);
});

test("callTool round-trips files in the workspace", async t => {
  const { workspace, toolkit } = setup(t);
  const wrote = await toolkit.callTool("write_file", { path: "notes.txt", content: "ship TODO\n" });
  assert.deepEqual({ name: wrote.name, ok: wrote.ok }, { name: "write_file", ok: true });
  assert.equal(readFileSync(join(workspace, "notes.txt"), "utf8"), "ship TODO\n");
  const found = await toolkit.callTool("grep_files", { pattern: "TODO" });
  assert.ok(found.ok, found.text);
  assert.match(found.text, /notes\.txt:1:ship TODO/);
});

test("tool errors are results; invalid requests throw", async t => {
  const { toolkit } = setup(t);
  const missing = await toolkit.callTool("read_file", { path: "absent.txt" });
  assert.equal(missing.ok, false);
  assert.match(missing.text, /^error:/);
  for (const name of ["terminal", "read_image", "no_such_tool"]) {
    assert.equal((await toolkit.callTool(name, {})).ok, false, name);
  }
  await assert.rejects(toolkit.callTool("", {}), TypeError);
  await assert.rejects(toolkit.callTool("read_file", ["x"]), TypeError);
  await assert.rejects(toolkit.callTool("read_file", { f: () => 1 }), TypeError);
  await assert.rejects(toolkit.callTool("read_file", {}, { permissionMode: "never" }), TypeError);
  await assert.rejects(toolkit.callTool("read_file", {}, { provider: "codex" }), TypeError);
});

test("permission mode and settings rules apply", async t => {
  const { workspace, toolkit } = setup(t);
  const asked = await toolkit.callTool("write_file", { path: "a.txt", content: "x" }, { permissionMode: "ask" });
  assert.equal(asked.ok, false);
  assert.match(asked.text, /permission required for write_file/);
  assert.equal(existsSync(join(workspace, "a.txt")), false);

  const ruled = setup(t, { permission_mode: "auto", permission: { edit: { "*deny*": "deny" } } });
  const denied = await ruled.toolkit.callTool("write_file", { path: "deny.txt", content: "x" });
  assert.match(denied.text, /permission denied for write_file/);
  assert.equal((await ruled.toolkit.callTool("write_file", { path: "ok.txt", content: "x" })).ok, true);
});

test("an aborted signal runs nothing", async t => {
  const { workspace, toolkit } = setup(t);
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(
    toolkit.callTool("write_file", { path: "c.txt", content: "x" }, { signal: controller.signal }),
    error => error instanceof TnyError && error.status === -12,
  );
  assert.equal(existsSync(join(workspace, "c.txt")), false);
});
