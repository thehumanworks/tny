#!/usr/bin/env node
import { createHash } from "node:crypto";
import { existsSync, mkdtempSync, readdirSync, readFileSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { pathToFileURL } from "node:url";

const NATIVE_NAMES = Object.freeze([
  "@thehumanworks/tny-darwin-arm64",
  "@thehumanworks/tny-linux-arm64",
  "@thehumanworks/tny-linux-x64",
]);
const META_NAME = "@thehumanworks/tny";

function npmFilename(name, version) {
  return `${name.replace(/^@/, "").replace("/", "-")}-${version}.tgz`;
}

function integrity(path) {
  return `sha512-${createHash("sha512").update(readFileSync(path)).digest("base64")}`;
}

function walk(root) {
  const result = [];
  for (const entry of readdirSync(root).sort()) {
    const path = join(root, entry);
    if (statSync(path).isDirectory()) result.push(...walk(path));
    else result.push(path);
  }
  return result;
}

export function discoverArtifacts(root, version) {
  const files = walk(root);
  const names = [...NATIVE_NAMES, META_NAME];
  return names.map((name) => {
    const filename = npmFilename(name, version);
    const matches = files.filter((path) => basename(path) === filename);
    if (!matches.length) throw new Error(`missing release artifact ${filename}`);
    if (name !== META_NAME && matches.length !== 1)
      throw new Error(`duplicate native release artifact ${filename}`);
    const values = new Set(matches.map(integrity));
    if (values.size !== 1) throw new Error(`non-identical duplicate release artifact ${filename}`);
    return Object.freeze({ name, version, path: matches[0], integrity: [...values][0] });
  });
}

function verifyMetadata(artifact, metadata) {
  if (!metadata || metadata.name !== artifact.name || metadata.version !== artifact.version ||
      metadata.dist?.integrity !== artifact.integrity) {
    throw new Error(
      `registry mismatch for ${artifact.name}@${artifact.version}: expected ${artifact.integrity}, ` +
      `got ${metadata?.dist?.integrity || "missing integrity"}`,
    );
  }
}

export async function publishRelease({ artifacts, lookup, publish, readback }) {
  const byName = new Map(artifacts.map((artifact) => [artifact.name, artifact]));
  const ordered = [...NATIVE_NAMES, META_NAME].map((name) => {
    const artifact = byName.get(name);
    if (!artifact) throw new Error(`missing planned artifact ${name}`);
    return artifact;
  });
  const statuses = [];
  for (const artifact of ordered) {
    const existing = await lookup(artifact);
    if (existing) {
      verifyMetadata(artifact, existing);
      statuses.push({ name: artifact.name, status: "already-present-identical" });
      continue;
    }
    await publish(artifact);
    const observed = await lookup(artifact, { afterPublish: true });
    verifyMetadata(artifact, observed);
    statuses.push({ name: artifact.name, status: "published-and-verified" });
  }
  await readback(ordered);
  return statuses;
}

function sleep(milliseconds) {
  return new Promise((resolvePromise) => setTimeout(resolvePromise, milliseconds));
}

function registryLookup(registry, attempts) {
  return async (artifact, options = {}) => {
    const limit = options.afterPublish ? attempts : 1;
    const url = `${registry}/${encodeURIComponent(artifact.name)}/${encodeURIComponent(artifact.version)}`;
    for (let attempt = 1; attempt <= limit; attempt++) {
      const response = await fetch(url, { headers: { accept: "application/json" } });
      if (response.status === 404) {
        if (options.afterPublish && attempt < limit) { await sleep(1000); continue; }
        return null;
      }
      if (!response.ok) throw new Error(`npm registry readback failed: HTTP ${response.status} for ${url}`);
      return await response.json();
    }
    return null;
  };
}

function npmPublish(registry) {
  return async (artifact) => {
    const result = spawnSync(
      "npm",
      ["publish", "--provenance", "--access", "public", "--registry", registry, artifact.path],
      { stdio: "inherit" },
    );
    if (result.error) throw result.error;
    if (result.status !== 0) throw new Error(`npm publish failed for ${artifact.name}@${artifact.version}`);
  };
}

function npmReadback(registry) {
  return async (artifacts) => {
    const meta = artifacts.at(-1);
    const root = mkdtempSync(join(tmpdir(), "tny-npm-readback-"));
    const workspace = join(root, "workspace");
    try {
      const init = spawnSync("npm", ["init", "-y"], { cwd: root, stdio: "ignore" });
      if (init.status !== 0) throw new Error("npm readback init failed");
      const install = spawnSync(
        "npm",
        ["install", "--ignore-scripts", "--no-audit", "--no-fund", "--registry", registry,
         `${meta.name}@${meta.version}`],
        { cwd: root, stdio: "inherit", env: { ...process.env, npm_config_cache: join(root, "cache") } },
      );
      if (install.status !== 0) throw new Error(`clean npm install readback failed for ${meta.name}@${meta.version}`);
      const makeWorkspace = spawnSync(process.execPath, ["-e", "require('fs').mkdirSync(process.argv[1])", workspace]);
      if (makeWorkspace.status !== 0) throw new Error("npm readback workspace creation failed");
      const program = [
        'import { Runtime } from "@thehumanworks/tny";',
        "const runtime = await Runtime.create({ workspace: process.argv[1] });",
        "if ((runtime.abiVersion >>> 16) > 1) throw new Error('unsupported ABI');",
        "await runtime.close();",
      ].join("\n");
      const loaded = spawnSync(process.execPath, ["--input-type=module", "-e", program, workspace], {
        cwd: root, stdio: "inherit",
      });
      if (loaded.status !== 0) throw new Error("clean npm import/native-load readback failed");
    } finally {
      const cleanup = spawnSync(process.execPath, ["-e", "require('fs').rmSync(process.argv[1], {recursive:true,force:true})", root]);
      if (cleanup.status !== 0) throw new Error("npm readback cleanup failed");
    }
  };
}

function argument(name, fallback) {
  const index = process.argv.indexOf(name);
  if (index < 0) return fallback;
  if (!process.argv[index + 1]) throw new Error(`missing value for ${name}`);
  return process.argv[index + 1];
}

async function main() {
  const root = resolve(argument("--root"));
  const version = argument("--version");
  const registry = argument("--registry", "https://registry.npmjs.org").replace(/\/$/, "");
  const attempts = Number(argument("--readback-attempts", "10"));
  if (!existsSync(root) || !version || !Number.isInteger(attempts) || attempts < 1)
    throw new Error("usage: publish_npm_release.mjs --root DIR --version VERSION [--registry URL]");
  const artifacts = discoverArtifacts(root, version);
  const statuses = await publishRelease({
    artifacts,
    lookup: registryLookup(registry, attempts),
    publish: npmPublish(registry),
    readback: npmReadback(registry),
  });
  process.stdout.write(`${JSON.stringify({ version, statuses })}\n`);
}

if (import.meta.url === pathToFileURL(process.argv[1] || "").href) {
  main().catch((error) => {
    process.stderr.write(`${error.stack || error}\n`);
    process.exitCode = 1;
  });
}
