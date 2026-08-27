import { createHash } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { spawnSync } from "node:child_process";
import { targetForHost } from "./platform.mjs";

const sha256 = (path) => createHash("sha256").update(readFileSync(path)).digest("hex");

function numericVersion(value) {
  if (typeof value !== "string" || !/^\d+(?:\.\d+)*$/.test(value)) return null;
  return value.split(".").map(Number);
}

function versionAtLeast(actual, required) {
  const left = numericVersion(actual);
  const right = numericVersion(required);
  if (!left || !right) return false;
  for (let index = 0; index < Math.max(left.length, right.length); index++) {
    if ((left[index] || 0) !== (right[index] || 0))
      return (left[index] || 0) > (right[index] || 0);
  }
  return true;
}

function hostMacVersion() {
  const result = spawnSync("sw_vers", ["-productVersion"], { encoding: "utf8" });
  return result.status === 0 ? result.stdout.trim() : undefined;
}

function fail(message, options) {
  const error = new Error(`@thehumanworks/tny: ${message}`, options);
  error.name = "TnyLoadError";
  return error;
}

function manifestAt(root) {
  const path = join(root, "prebuilds", "manifest.json");
  try {
    return { path, value: JSON.parse(readFileSync(path, "utf8")) };
  } catch (cause) {
    throw fail(`native package manifest is missing or invalid at ${path}`, { cause });
  }
}

function assertBinaryTarget(path, target) {
  const bytes = readFileSync(path);
  if (target.platform === "darwin") {
    if (bytes.length < 12 || bytes.readUInt32LE(0) !== 0xfeedfacf ||
        bytes.readUInt32LE(4) !== 0x0100000c) {
      throw fail(`${target.packageName} contains a non-arm64 Mach-O artifact: ${path}`);
    }
    return;
  }
  const machine = target.architecture === "x64" ? 62 : 183;
  if (bytes.length < 20 || bytes[0] !== 0x7f || bytes.toString("ascii", 1, 4) !== "ELF" ||
      bytes[4] !== 2 || bytes[5] !== 1 || bytes.readUInt16LE(18) !== machine) {
    throw fail(`${target.packageName} contains a non-${target.architecture} ELF64 artifact: ${path}`);
  }
}

export function verifyNativePackage(root, target, expectedVersion, host = {}) {
  let packageJson;
  try {
    packageJson = JSON.parse(readFileSync(join(root, "package.json"), "utf8"));
  } catch (cause) {
    throw fail(`native package metadata is missing or invalid at ${root}`, { cause });
  }
  const { value: manifest } = manifestAt(root);
  const addonPath = join(root, "prebuilds", "tny.node");
  const libraryPath = join(root, "prebuilds", target.library);
  if (packageJson.name !== target.packageName || packageJson.version !== expectedVersion) {
    throw fail(
      `native package identity mismatch: expected ${target.packageName}@${expectedVersion}, ` +
      `got ${packageJson.name || "unknown"}@${packageJson.version || "unknown"}`,
    );
  }
  if (manifest.schemaVersion !== 3 || manifest.packageName !== target.packageName ||
      manifest.packageVersion !== expectedVersion || manifest.platform !== target.platform ||
      manifest.architecture !== target.architecture || manifest.abiMajor !== 1 ||
      !Number.isInteger(manifest.abiMinor) || manifest.abiMinor < 0 ||
      manifest.libraryVersion !== expectedVersion ||
      !Number.isInteger(manifest.capabilityStructSize) || manifest.capabilityStructSize <= 0 ||
      manifest.linkage !== "shared-loader-relative" ||
      manifest.dynamicIdentity?.value !== (
        target.platform === "darwin" ? "@rpath/libtny.1.dylib" : "libtny.so.1"
      )) {
    throw fail(`${target.packageName}@${expectedVersion} has incompatible artifact metadata`);
  }
  if (!existsSync(addonPath) || !existsSync(libraryPath)) {
    throw fail(`${target.packageName}@${expectedVersion} is incomplete; expected addon and ${target.library}`);
  }
  if (sha256(addonPath) !== manifest.addonSha256 ||
      sha256(libraryPath) !== manifest.librarySha256) {
    throw fail(`${target.packageName}@${expectedVersion} failed SHA-256 integrity validation`);
  }
  assertBinaryTarget(addonPath, target);
  assertBinaryTarget(libraryPath, target);
  if (target.platform === "darwin") {
    const current = host.macVersion || hostMacVersion();
    if (!current || !versionAtLeast(current, manifest.minimumOs)) {
      throw fail(
        `${target.packageName}@${expectedVersion} requires macOS ` +
        `${manifest.minimumOs || "unknown"} or newer`,
      );
    }
  } else {
    const current = host.glibcVersion;
    if (!versionAtLeast("2.34", manifest.minimumGlibc)) {
      throw fail(
        `${target.packageName}@${expectedVersion} has unsupported glibc floor ` +
        `${manifest.minimumGlibc || "unknown"}; release maximum is 2.34`,
      );
    }
    if (!current || !versionAtLeast(current, manifest.minimumGlibc)) {
      throw fail(
        `${target.packageName}@${expectedVersion} requires glibc ` +
        `${manifest.minimumGlibc || "unknown"} or newer (host: ${current || "unknown"})`,
      );
    }
  }
  return Object.freeze({ addonPath, libraryPath, manifest });
}

function verifyCheckoutBuild(packageRoot, target) {
  const root = join(packageRoot, "build", "Release");
  const manifestPath = join(root, "manifest.json");
  const addonPath = join(root, "tny.node");
  const libraryPath = join(root, target.library);
  if (![manifestPath, addonPath, libraryPath].every(existsSync)) return null;
  let manifest;
  try { manifest = JSON.parse(readFileSync(manifestPath, "utf8")); }
  catch (cause) { throw fail(`checkout build manifest is invalid at ${manifestPath}`, { cause }); }
  if (manifest.platform !== target.platform || manifest.architecture !== target.architecture ||
      manifest.schemaVersion !== 2 || manifest.abiMajor !== 1 || manifest.abiMinor < 0 ||
      sha256(addonPath) !== manifest.addonSha256 ||
      sha256(libraryPath) !== manifest.librarySha256) {
    throw fail("checkout build does not match this host or its integrity manifest");
  }
  const expectedIdentity = target.platform === "darwin"
    ? { kind: "install_name", value: "@rpath/libtny.1.dylib" }
    : { kind: "soname", value: "libtny.so.1" };
  if (JSON.stringify(manifest.dynamicIdentity) !== JSON.stringify(expectedIdentity))
    throw fail("checkout build has the wrong libtny ABI 1 dynamic identity");
  assertBinaryTarget(addonPath, target);
  assertBinaryTarget(libraryPath, target);
  return Object.freeze({ addonPath, libraryPath, manifest });
}

export function resolveNativeAddon({
  packageRoot,
  platform = process.platform,
  architecture = process.arch,
  glibcVersion = process.report?.getReport?.().header?.glibcVersionRuntime,
  nativePackageRoot = process.env.TNY_NATIVE_PACKAGE_DIR,
  resolvePackage,
  allowCheckoutBuild = true,
} = {}) {
  let target;
  try { target = targetForHost(platform, architecture, glibcVersion); }
  catch (cause) { throw fail(cause.message, { cause }); }
  const meta = JSON.parse(readFileSync(join(packageRoot, "package.json"), "utf8"));
  const host = { glibcVersion };
  if (nativePackageRoot)
    return verifyNativePackage(nativePackageRoot, target, meta.version, host);
  if (allowCheckoutBuild) {
    const checkout = verifyCheckoutBuild(packageRoot, target);
    if (checkout) return checkout;
  }
  const resolver = resolvePackage || (() => {
    const require = createRequire(join(packageRoot, "package.json"));
    return dirname(require.resolve(`${target.packageName}/package.json`));
  });
  let root;
  try { root = resolver(target.packageName); }
  catch (cause) {
    throw fail(
      `missing optional native package ${target.packageName}@${meta.version} for ` +
      `${platform}-${architecture}; reinstall without --no-optional, or set ` +
      "TNY_NATIVE_PACKAGE_DIR to an exact unpacked platform package",
      { cause },
    );
  }
  if (typeof root !== "string") {
    throw fail(`${target.packageName}@${meta.version} did not resolve to its package root`);
  }
  return verifyNativePackage(root, target, meta.version, host);
}
