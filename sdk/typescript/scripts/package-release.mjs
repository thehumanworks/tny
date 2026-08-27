#!/usr/bin/env node
import { createHash } from "node:crypto";
import {
  copyFileSync, cpSync, mkdirSync, mkdtempSync, readFileSync, rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import { targetForTriple, targets } from "./platform.mjs";
import { versionsFromTag } from "./release-version.mjs";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = resolve(packageRoot, "../..");
const sha256 = (path) => createHash("sha256").update(readFileSync(path)).digest("hex");

function argument(name, fallback) {
  const index = process.argv.indexOf(name);
  if (index < 0) {
    if (fallback !== undefined) return fallback;
    throw new Error(`missing required ${name}`);
  }
  if (!process.argv[index + 1]) throw new Error(`missing required ${name}`);
  return process.argv[index + 1];
}

function copy(relative, destination) {
  const target = join(destination, relative);
  mkdirSync(dirname(target), { recursive: true });
  cpSync(join(packageRoot, relative), target, { recursive: true });
}

function pack(stage, output) {
  const npmEnvironment = {
    ...process.env,
    npm_config_cache: join(scratch, "npm-cache"),
    npm_config_audit: "false",
    npm_config_fund: "false",
    npm_config_ignore_scripts: "true",
    npm_config_offline: "true",
  };
  const dryRun = spawnSync("npm", ["pack", "--dry-run", "--json"], {
    cwd: stage, encoding: "utf8", env: npmEnvironment,
  });
  if (dryRun.status !== 0) throw new Error(dryRun.stderr || "npm pack --dry-run failed");
  let report;
  try { report = JSON.parse(dryRun.stdout)[0]; }
  catch { throw new Error(`npm pack --dry-run returned invalid JSON: ${dryRun.stdout}`); }
  if (!Array.isArray(report?.files) || report.files.some((entry) =>
    entry.path.includes("build/Release") || entry.path.startsWith("native/"))) {
    throw new Error("release package content audit failed");
  }
  const result = spawnSync("npm", ["pack", "--json", "--pack-destination", output], {
    cwd: stage, encoding: "utf8", env: npmEnvironment,
  });
  if (result.status !== 0) throw new Error(result.stderr || "npm pack failed");
  const packed = JSON.parse(result.stdout)[0];
  return { path: join(output, packed.filename), files: report.files.map((entry) => entry.path) };
}

function spdx(name, version, packageFile, artifactFiles, commit) {
  const namespaceHash = sha256(packageFile);
  return {
    spdxVersion: "SPDX-2.3",
    dataLicense: "CC0-1.0",
    SPDXID: "SPDXRef-DOCUMENT",
    name: `${name}-${version}`,
    comment: `source commit ${commit}`,
    documentNamespace: `https://github.com/thehumanworks/tny/sbom/${namespaceHash}`,
    creationInfo: {
      created: new Date().toISOString(),
      creators: ["Tool: sdk/typescript/scripts/package-release.mjs"],
    },
    packages: [{
      SPDXID: "SPDXRef-Package",
      name,
      versionInfo: version,
      downloadLocation: "NOASSERTION",
      filesAnalyzed: true,
      licenseConcluded: "NOASSERTION",
      licenseDeclared: "NOASSERTION",
      checksums: [{ algorithm: "SHA256", checksumValue: namespaceHash }],
    }],
    files: artifactFiles.map((file, index) => ({
      SPDXID: `SPDXRef-File-${index + 1}`,
      fileName: file.name,
      checksums: [{ algorithm: "SHA256", checksumValue: file.sha256 }],
      licenseConcluded: "NOASSERTION",
    })),
    relationships: artifactFiles.map((_file, index) => ({
      spdxElementId: "SPDXRef-Package",
      relationshipType: "CONTAINS",
      relatedSpdxElement: `SPDXRef-File-${index + 1}`,
    })),
  };
}

const tag = argument("--tag");
const target = targetForTriple(argument("--triple"));
const output = resolve(argument("--out"));
const wheel = resolve(argument("--wheel"));
const commit = argument("--commit", process.env.GITHUB_SHA);
const versions = versionsFromTag(tag);
if (!/^[0-9a-f]{40}$/.test(commit || ""))
  throw new Error("--commit must be the exact 40-character lowercase source commit SHA");
const wheelList = spawnSync("unzip", ["-Z1", wheel], { encoding: "utf8" });
if (wheelList.status !== 0) throw new Error(`cannot inspect Python wheel ${wheel}`);
const wheelContents = wheelList.stdout.trim().split("\n").filter(Boolean).sort();
const npmTool = spawnSync("npm", ["--version"], { encoding: "utf8" });
if (npmTool.status !== 0) throw new Error("cannot determine npm version");
mkdirSync(output, { recursive: true });
const scratch = mkdtempSync(join(tmpdir(), "tny-node-release-"));

try {
  const base = JSON.parse(readFileSync(join(packageRoot, "package.json"), "utf8"));
  const buildRoot = join(packageRoot, "build", "Release");
  const addon = join(buildRoot, "tny.node");
  const library = join(buildRoot, target.library);
  const sourceManifest = JSON.parse(readFileSync(join(buildRoot, "manifest.json"), "utf8"));
  if (sourceManifest.platform !== target.platform ||
      sourceManifest.architecture !== target.architecture) {
    throw new Error(
      `build manifest is ${sourceManifest.platform}-${sourceManifest.architecture}, ` +
      `not requested ${target.platform}-${target.architecture}`,
    );
  }
  if (sourceManifest.libraryVersion !== versions.npm) {
    throw new Error(
      `libtny reports ${sourceManifest.libraryVersion || "unknown"}, ` +
      `but release tag ${versions.tag} requires ${versions.npm}`,
    );
  }
  const metaStage = join(scratch, "meta", "package");
  mkdirSync(metaStage, { recursive: true });
  for (const relative of [
    "dist", "scripts/native-loader.mjs", "scripts/platform.mjs", "README.md",
    "LICENSE-METADATA.json", "THIRD_PARTY_NOTICES.md",
  ]) copy(relative, metaStage);
  const rootLicenses = ["LICENSE", "LICENSE.txt", "LICENSE.md"]
    .map((name) => join(repositoryRoot, name)).filter(existsSync);
  if (rootLicenses.length > 1) throw new Error("repository has multiple candidate root licenses");
  if (rootLicenses.length === 1) copyFileSync(rootLicenses[0], join(metaStage, "LICENSE"));
  const optionalDependencies = Object.fromEntries(
    targets.map((candidate) => [candidate.packageName, versions.npm]),
  );
  const meta = {
    name: base.name,
    version: versions.npm,
    description: base.description,
    type: "module",
    main: "./dist/index.mjs",
    types: "./dist/index.d.ts",
    exports: base.exports,
    optionalDependencies,
    engines: base.engines,
    keywords: base.keywords,
    license: base.license,
    repository: { type: "git", url: "git+https://github.com/thehumanworks/tny.git" },
    bugs: { url: "https://github.com/thehumanworks/tny/issues" },
    homepage: "https://github.com/thehumanworks/tny#readme",
  };
  writeFileSync(join(metaStage, "package.json"), `${JSON.stringify(meta, null, 2)}\n`);
  const metaPack = pack(metaStage, output);

  const nativeStage = join(scratch, "native", "package");
  const prebuilds = join(nativeStage, "prebuilds");
  mkdirSync(prebuilds, { recursive: true });
  copyFileSync(addon, join(prebuilds, "tny.node"));
  copyFileSync(library, join(prebuilds, target.library));
  const nativeManifest = {
    ...sourceManifest,
    schemaVersion: 2,
    packageName: target.packageName,
    packageVersion: versions.npm,
    releaseTag: versions.tag,
  };
  writeFileSync(join(prebuilds, "manifest.json"), `${JSON.stringify(nativeManifest, null, 2)}\n`);
  for (const relative of ["LICENSE-METADATA.json", "THIRD_PARTY_NOTICES.md"])
    copy(relative, nativeStage);
  if (rootLicenses.length === 1) copyFileSync(rootLicenses[0], join(nativeStage, "LICENSE"));
  writeFileSync(join(nativeStage, "README.md"),
    `# ${target.packageName}\n\nNative payload for @thehumanworks/tny on ${target.platform}-${target.architecture}.\n`);
  const native = {
    name: target.packageName,
    version: versions.npm,
    description: `Native ${target.platform}-${target.architecture} payload for @thehumanworks/tny`,
    exports: { "./package.json": "./package.json" },
    os: [target.platform],
    cpu: [target.architecture],
    ...(target.platform === "linux" ? { libc: ["glibc"] } : {}),
    engines: base.engines,
    license: base.license,
    repository: { type: "git", url: "git+https://github.com/thehumanworks/tny.git" },
  };
  writeFileSync(join(nativeStage, "package.json"), `${JSON.stringify(native, null, 2)}\n`);
  const nativePack = pack(nativeStage, output);

  const artifacts = [
    { name: basename(metaPack.path), sha256: sha256(metaPack.path), role: "npm-meta-package" },
    { name: basename(nativePack.path), sha256: sha256(nativePack.path), role: "npm-native-package" },
    { name: "tny.node", packagePath: "package/prebuilds/tny.node", sha256: sha256(addon), role: "node-api-addon" },
    { name: target.library, packagePath: `package/prebuilds/${target.library}`, sha256: sha256(library), role: "libtny-shared-library" },
    { name: basename(wheel), sha256: sha256(wheel), role: "python-platform-wheel" },
  ];
  const descriptor = {
    schemaVersion: 1,
    releaseTag: versions.tag,
    npmVersion: versions.npm,
    pythonVersion: versions.python,
    target,
    packages: artifacts.slice(0, 2),
    nativeArtifacts: artifacts.slice(2, 4),
    pythonWheel: artifacts[4],
    commitSha: commit,
    buildTools: { node: process.version, npm: npmTool.stdout.trim() },
    contents: { meta: metaPack.files, native: nativePack.files, wheel: wheelContents },
  };
  const prefix = `tny-node-${target.triple}-${versions.npm}`;
  writeFileSync(join(output, `${prefix}.provenance.json`),
    `${JSON.stringify(descriptor, null, 2)}\n`);
  writeFileSync(join(output, `${prefix}.spdx.json`),
    `${JSON.stringify(spdx(target.packageName, versions.npm, nativePack.path, artifacts.slice(2), commit), null, 2)}\n`);
  process.stdout.write(`${JSON.stringify(descriptor)}\n`);
} finally {
  rmSync(scratch, { recursive: true, force: true });
}
