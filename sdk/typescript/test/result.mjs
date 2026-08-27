import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const resultDir = join(packageRoot, "build/conformance");

export function recordResult(id, status, detail) {
  mkdirSync(resultDir, { recursive: true });
  writeFileSync(join(resultDir, `${id}.json`), `${JSON.stringify({ id, status, ...detail }, null, 2)}\n`);
}

export { resultDir };
