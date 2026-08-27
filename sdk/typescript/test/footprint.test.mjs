import assert from "node:assert/strict";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import test from "node:test";

test("footprint command records addon, library and tgz sizes", () => {
  const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
  const run = spawnSync(process.execPath, [join(packageRoot, "scripts/footprint.mjs")], {
    cwd: packageRoot, encoding: "utf8",
  });
  assert.equal(run.status, 0, run.stderr);
  const footprint = JSON.parse(run.stdout);
  for (const artifact of [footprint.addon, footprint.library, footprint.package]) {
    assert.ok(artifact.bytes > 0);
    assert.match(artifact.sha256, /^[0-9a-f]{64}$/);
  }
});
