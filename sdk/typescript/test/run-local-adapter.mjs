import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const artifact = join(packageRoot, "build/Release/tny.node");
const sha256 = createHash("sha256").update(readFileSync(artifact)).digest("hex");
const request = {
  adapter_protocol_version: 1,
  conformance_version: 1,
  contract_sha256: "0".repeat(64),
  artifact: { path: artifact, sha256 },
  secret_sentinel: "tny-local-conformance-secret",
};
const run = spawnSync(process.execPath, [join(packageRoot, "test/conformance-adapter.mjs")], {
  cwd: packageRoot, input: `${JSON.stringify(request)}\n`, encoding: "utf8",
});
if (run.stderr) process.stderr.write(run.stderr);
if (run.stdout) process.stdout.write(run.stdout);
process.exit(run.status ?? 127);
