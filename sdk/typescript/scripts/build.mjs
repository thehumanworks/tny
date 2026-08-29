import { createHash } from "node:crypto";
import {
  copyFileSync, existsSync, mkdirSync, readFileSync, realpathSync, rmSync, writeFileSync,
} from "node:fs";
import { createRequire } from "node:module";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const inferredRoot = resolve(packageRoot, "../..");
const sourceCheckout = existsSync(join(inferredRoot, "Makefile")) &&
  existsSync(join(inferredRoot, "include/tny/tny.h"));
const outputDir = join(packageRoot, "build/Release");
const output = join(outputDir, "tny.node");
const manifestPath = join(outputDir, "manifest.json");
const libName = process.platform === "darwin" ? "libtny.1.dylib" : "libtny.so.1";
const bundledLibrary = join(outputDir, libName);
const legacyLibrary = join(
  outputDir, process.platform === "darwin" ? "libtny.0.dylib" : "libtny.so.0"
);
rmSync(legacyLibrary, { force: true });
const requiredAbiMajor = 1;
const minAbiMinor = 0;
const glibcVersion = process.report?.getReport?.().header?.glibcVersionRuntime;
const glibc = process.platform !== "linux" || Boolean(glibcVersion);
const supported =
  (process.platform === "darwin" && process.arch === "arm64") ||
  (glibc && process.platform === "linux" && (process.arch === "x64" || process.arch === "arm64"));

if (!supported) {
  throw new Error(
    `@thehumanworks/tny: unsupported platform ${process.platform}-${process.arch}; ` +
    "the native ABI supports darwin-arm64 and glibc linux-x64/linux-arm64",
  );
}

const sourceFiles = [
  "native/addon.c", "native/addon_internal.h", "native/copy.c", "native/events.c",
  "native/options.c", "native/owner.c", "native/probe.c", "scripts/build.mjs",
];
const digest = (value) => createHash("sha256").update(value).digest("hex");
const fileHash = (path) => digest(readFileSync(path));
const sourceHash = () => {
  const hash = createHash("sha256");
  for (const relative of sourceFiles) hash.update(readFileSync(join(packageRoot, relative)));
  return hash.digest("hex");
};

function artifactMetadata() {
  const raw = process.env.TNY_ARTIFACT_METADATA || process.env.ARTIFACT_METADATA;
  if (!raw) return {};
  return JSON.parse(existsSync(raw) ? readFileSync(raw, "utf8") : raw);
}

const provenance = artifactMetadata();
const expectedLibrarySha = process.env.TNY_EXPECTED_LIB_SHA256 || provenance.sha256;
function validateLibraryProvenance(path) {
  const actual = fileHash(path);
  const metadataPlatform = provenance.platform || provenance.os;
  const metadataArchitecture = provenance.architecture || provenance.arch;
  const expectedIdentity = process.platform === "darwin"
    ? { kind: "install_name", value: "@rpath/libtny.1.dylib" }
    : { kind: "soname", value: "libtny.so.1" };
  if (expectedLibrarySha && actual !== expectedLibrarySha)
    throw new Error("staged libtny SHA-256 does not match release provenance");
  if (provenance.filename && provenance.filename !== libName)
    throw new Error("staged libtny filename metadata does not match this build");
  if (metadataPlatform && metadataPlatform !== process.platform)
    throw new Error("staged libtny platform metadata does not match this build");
  const normalizedArchitecture = process.arch === "x64" ? "x86_64"
    : process.arch === "arm64" && process.platform === "linux" ? "aarch64"
    : process.arch;
  if (metadataArchitecture && metadataArchitecture !== normalizedArchitecture)
    throw new Error("staged libtny architecture metadata does not match this build");
  if (Object.keys(provenance).length &&
      (provenance.schema_version !== 2 || provenance.abi_major !== requiredAbiMajor ||
       JSON.stringify(provenance.dynamic_identity) !== JSON.stringify(expectedIdentity)))
    throw new Error("staged libtny ABI/dynamic identity metadata does not match this build");
  return actual;
}

function versionParts(value) {
  return String(value).split(".").map((part) => Number.parseInt(part, 10) || 0);
}

function versionAtLeast(actual, required) {
  const a = versionParts(actual);
  const b = versionParts(required);
  for (let index = 0; index < Math.max(a.length, b.length); index++) {
    if ((a[index] || 0) !== (b[index] || 0)) return (a[index] || 0) > (b[index] || 0);
  }
  return true;
}

function compareVersions(left, right) {
  if (versionAtLeast(left, right) && versionAtLeast(right, left)) return 0;
  return versionAtLeast(left, right) ? 1 : -1;
}

function validateBinary(path, architecture) {
  const bytes = readFileSync(path);
  if (process.platform === "darwin") {
    if (bytes.length < 12 || bytes.readUInt32LE(0) !== 0xfeedfacf ||
        bytes.readUInt32LE(4) !== 0x0100000c || architecture !== "arm64") {
      throw new Error(`${path} is not an arm64 Mach-O 64-bit binary`);
    }
  } else {
    const machine = architecture === "x64" ? 62 : 183;
    if (bytes.length < 20 || bytes[0] !== 0x7f || bytes.toString("ascii", 1, 4) !== "ELF" ||
        bytes[4] !== 2 || bytes[5] !== 1 || bytes.readUInt16LE(18) !== machine) {
      throw new Error(`${path} is not a ${architecture} little-endian ELF64 binary`);
    }
  }
}

function currentMacVersion() {
  const result = spawnSync("sw_vers", ["-productVersion"], { encoding: "utf8" });
  return result.status === 0 ? result.stdout.trim() : undefined;
}

function binaryMacMinimum(path) {
  const result = spawnSync("otool", ["-l", path], { encoding: "utf8" });
  const match = result.status === 0 ? result.stdout.match(/\bminos\s+([0-9.]+)/) : null;
  return match?.[1];
}

function binaryGlibcFloor(paths) {
  if (process.env.TNY_RELEASE_GLIBC_FLOOR) return process.env.TNY_RELEASE_GLIBC_FLOOR;
  const versions = [];
  for (const path of paths) {
    const result = spawnSync("readelf", ["--version-info", path], { encoding: "utf8" });
    if (result.status !== 0) throw new Error(
      "readelf is required to derive the Linux release glibc floor; " +
      "set TNY_RELEASE_GLIBC_FLOOR only to a separately verified release value",
    );
    for (const match of result.stdout.matchAll(/GLIBC_([0-9]+(?:\.[0-9]+)+)/g))
      versions.push(match[1]);
  }
  if (!versions.length) return "0";
  return versions.sort(compareVersions).at(-1);
}

async function probeAddon(path) {
  const require = createRequire(import.meta.url);
  const addon = require(path);
  return await addon.__probeAbi();
}

async function validatePrebuilt() {
  if (sourceCheckout && process.env.TNY_USE_PREBUILT !== "1") return false;
  if (!existsSync(manifestPath) || !existsSync(output) || !existsSync(bundledLibrary)) return false;
  const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
  if (manifest.platform !== process.platform || manifest.architecture !== process.arch)
    throw new Error("prebuilt platform or architecture does not match this host");
  const expectedDynamicIdentity = process.platform === "darwin"
    ? { kind: "install_name", value: "@rpath/libtny.1.dylib" }
    : { kind: "soname", value: "libtny.so.1" };
  if (JSON.stringify(manifest.dynamicIdentity) !== JSON.stringify(expectedDynamicIdentity))
    throw new Error("prebuilt dynamic identity does not match libtny ABI 1");
  if (manifest.sourceSha256 !== sourceHash())
    throw new Error("prebuilt source hash does not match the packaged addon sources");
  if (manifest.schemaVersion !== 2 || manifest.abiMajor !== requiredAbiMajor ||
      manifest.abiMinor < minAbiMinor)
    throw new Error(`prebuilt libtny ABI ${manifest.abiMajor}.${manifest.abiMinor} is unsupported`);
  if (manifest.addonSha256 !== fileHash(output) || manifest.librarySha256 !== fileHash(bundledLibrary))
    throw new Error("prebuilt artifact hash mismatch");
  validateLibraryProvenance(bundledLibrary);
  validateBinary(output, process.arch);
  validateBinary(bundledLibrary, process.arch);
  if (process.platform === "darwin") {
    const current = currentMacVersion();
    if (!manifest.minimumOs || !current || !versionAtLeast(current, manifest.minimumOs))
      throw new Error(`prebuilt requires macOS ${manifest.minimumOs || "unknown"} or newer`);
  } else if (!manifest.minimumGlibc || !versionAtLeast(glibcVersion, manifest.minimumGlibc)) {
    throw new Error(`prebuilt requires glibc ${manifest.minimumGlibc || "unknown"} or newer`);
  }
  if (process.platform === "linux" && !versionAtLeast("2.34", manifest.minimumGlibc))
    throw new Error(`prebuilt glibc floor ${manifest.minimumGlibc} exceeds release maximum 2.34`);
  const probe = await probeAddon(output);
  if ((probe.abiVersion >>> 16) !== manifest.abiMajor ||
      (probe.abiVersion & 0xffff) !== manifest.abiMinor ||
      probe.capabilitySize !== manifest.capabilityStructSize ||
      probe.libraryVersion !== manifest.libraryVersion) {
    throw new Error("prebuilt ABI/capability probe does not match its manifest");
  }
  console.log(`@thehumanworks/tny: using verified prebuilt ${process.platform}-${process.arch}`);
  return true;
}

let prebuiltFailure;
try {
  if (await validatePrebuilt()) process.exit(0);
} catch (error) {
  prebuiltFailure = error;
}

const tnyRoot = resolve(process.env.TNY_ROOT || inferredRoot);
const includeDir = resolve(process.env.TNY_INCLUDE_DIR || join(tnyRoot, "include"));
const libDir = resolve(process.env.TNY_LIB_DIR || join(tnyRoot, "build/lib"));
const header = join(includeDir, "tny/tny.h");
const library = join(libDir, libName);
if (!existsSync(header)) {
  const detail = prebuiltFailure ? ` Prebuilt validation failed: ${prebuiltFailure.message}.` : "";
  throw new Error(
    `@thehumanworks/tny: no valid prebuilt and libtny header not found at ${header}.` +
    `${detail} Set TNY_ROOT or TNY_INCLUDE_DIR/TNY_LIB_DIR for a source fallback.`,
  );
}
if (!process.env.TNY_LIB_DIR && existsSync(join(tnyRoot, "Makefile"))) {
  const made = spawnSync("make", ["lib-shared"], { cwd: tnyRoot, stdio: "inherit" });
  if (made.status !== 0) process.exit(made.status ?? 1);
}
if (!existsSync(library)) {
  throw new Error(`@thehumanworks/tny: ${libName} not found at ${library}`);
}
validateLibraryProvenance(library);
const headerText = readFileSync(header, "utf8");
const abiMajor = Number(headerText.match(/#define\s+TNY_ABI_MAJOR\s+(\d+)u/)?.[1]);
const abiMinor = Number(headerText.match(/#define\s+TNY_ABI_MINOR\s+(\d+)u/)?.[1]);
if (abiMajor !== requiredAbiMajor || !Number.isInteger(abiMinor) || abiMinor < minAbiMinor)
  throw new Error(`@thehumanworks/tny: libtny ABI ${abiMajor}.${abiMinor} is unsupported; need 1.0+ within major 1`);

const nodeRoot = resolve(dirname(realpathSync(process.execPath)), "..");
const nodeInclude = join(nodeRoot, "include/node");
if (!existsSync(join(nodeInclude, "node_api.h")))
  throw new Error(`@thehumanworks/tny: Node-API headers not found at ${nodeInclude}`);

mkdirSync(outputDir, { recursive: true });
const cc = process.env.CC || "cc";
const args = [
  "-std=c11", "-O2", "-fPIC", "-fvisibility=hidden",
  "-Wall", "-Wextra", "-Werror", `-I${nodeInclude}`, `-I${includeDir}`,
  ...sourceFiles.filter((path) => path.endsWith(".c")).map((path) => join(packageRoot, path)),
  `-L${libDir}`, "-ltny",
];
if (process.platform === "darwin")
  args.push("-bundle", "-undefined", "dynamic_lookup", "-Wl,-rpath,@loader_path", "-o", output);
else
  args.push("-shared", "-pthread", "-lm", "-ldl", "-Wl,-rpath,$ORIGIN", "-o", output);
const built = spawnSync(cc, args, { cwd: packageRoot, stdio: "inherit" });
if (built.error) throw built.error;
if (built.status !== 0) process.exit(built.status ?? 1);
copyFileSync(library, bundledLibrary);
validateBinary(output, process.arch);
validateBinary(bundledLibrary, process.arch);
const probe = await probeAddon(output);
const probedMajor = probe.abiVersion >>> 16;
const probedMinor = probe.abiVersion & 0xffff;
if (probedMajor !== requiredAbiMajor || probedMinor < minAbiMinor || probe.capabilitySize === 0)
  throw new Error(`@thehumanworks/tny: built artifact failed ABI/capability validation`);
if (provenance.library_version && probe.libraryVersion !== provenance.library_version)
  throw new Error("built addon library version does not match release provenance");
const minimumOs = process.platform === "darwin"
  ? [binaryMacMinimum(output), binaryMacMinimum(bundledLibrary)].filter(Boolean)
      .sort(compareVersions).at(-1)
  : undefined;
const minimumGlibc = process.platform === "linux"
  ? binaryGlibcFloor([output, bundledLibrary])
  : undefined;
if (minimumGlibc && !versionAtLeast("2.34", minimumGlibc))
  throw new Error(`derived glibc floor ${minimumGlibc} exceeds release maximum 2.34`);
const manifest = {
  schemaVersion: 2,
  platform: process.platform,
  architecture: process.arch,
  abiMajor: probedMajor,
  abiMinor: probedMinor,
  libraryVersion: probe.libraryVersion,
  capabilityStructSize: probe.capabilitySize,
  addonSha256: fileHash(output),
  librarySha256: fileHash(bundledLibrary),
  sourceSha256: sourceHash(),
  minimumOs,
  minimumGlibc,
  linkage: "shared-loader-relative",
  dynamicIdentity: provenance.dynamic_identity || (
    process.platform === "darwin"
      ? { kind: "install_name", value: "@rpath/libtny.1.dylib" }
      : { kind: "soname", value: "libtny.so.1" }
  ),
  provenance: {
    expectedLibrarySha256: expectedLibrarySha || null,
    artifactId: provenance.artifact_id || provenance.id || null,
  },
};
writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, { mode: 0o644 });
console.log(`@thehumanworks/tny: built and verified ${output}`);
