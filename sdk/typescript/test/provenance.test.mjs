import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, realpathSync, rmSync, symlinkSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import test from "node:test";

test("staged library provenance accepts exact SHA and rejects mismatch", () => {
  const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
  const libraryName = process.platform === "darwin" ? "libtny.1.dylib" : "libtny.so.1";
  const library = join(packageRoot, "build/Release", libraryName);
  const sha = createHash("sha256").update(readFileSync(library)).digest("hex");
  const run = (expected) => spawnSync(process.execPath, [join(packageRoot, "scripts/build.mjs")], {
    cwd: packageRoot, encoding: "utf8",
    env: { ...process.env, TNY_EXPECTED_LIB_SHA256: expected },
  });
  assert.notEqual(run("0".repeat(64)).status, 0);
  assert.equal(run(sha).status, 0);
});

test("Node header override accepts a separate prefix and rejects missing headers", () => {
  const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
  const headers = process.env.TNY_NODE_INCLUDE ||
    join(resolve(dirname(realpathSync(process.execPath)), ".."), "include/node");
  const temporary = mkdtempSync(join(tmpdir(), "tny-node-headers-"));
  try {
    const include = join(temporary, "include");
    symlinkSync(headers, include, "dir");
    const run = (nodeInclude) => spawnSync(process.execPath, [join(packageRoot, "scripts/build.mjs")], {
      cwd: packageRoot, encoding: "utf8",
      env: { ...process.env, TNY_NODE_INCLUDE: nodeInclude },
    });
    const missing = run(join(temporary, "missing"));
    assert.notEqual(missing.status, 0);
    assert.match(missing.stderr, /Node-API headers not found at/);
    const valid = run(include);
    assert.equal(valid.status, 0, valid.stderr);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});
