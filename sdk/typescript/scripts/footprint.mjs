import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const release = join(packageRoot, "build/Release");
const libraryName = process.platform === "darwin" ? "libtny.0.dylib" : "libtny.so.0";
const temporary = mkdtempSync(join(tmpdir(), "tny-node-footprint-"));
const packed = spawnSync("npm", ["pack", "--json", "--pack-destination", temporary], {
  cwd: packageRoot, encoding: "utf8",
});
if (packed.status !== 0) throw new Error("npm pack failed while measuring footprint");
const jsonStart = packed.stdout.lastIndexOf("[\n  {");
if (jsonStart < 0) throw new Error("npm pack did not return JSON metadata");
const archive = join(temporary, JSON.parse(packed.stdout.slice(jsonStart))[0].filename);
const describe = (path) => ({
  bytes: statSync(path).size,
  sha256: createHash("sha256").update(readFileSync(path)).digest("hex"),
});
process.stdout.write(`${JSON.stringify({
  platform: `${process.platform}-${process.arch}`,
  addon: describe(join(release, "tny.node")),
  library: describe(join(release, libraryName)),
  package: describe(archive),
})}\n`);
