import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
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
