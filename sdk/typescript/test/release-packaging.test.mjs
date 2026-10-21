import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { chmodSync, mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { resolveNativeAddon, verifyNativeExecutor, verifyNativePackage } from "../scripts/native-loader.mjs";
import { targetForHost } from "../scripts/platform.mjs";
import { versionsFromTag } from "../scripts/release-version.mjs";

const hash = (path) => createHash("sha256").update(readFileSync(path)).digest("hex");

test("release tags produce exact canonical npm and PEP 440 versions", () => {
  assert.deepEqual(versionsFromTag("v1.2.3"), { tag: "v1.2.3", npm: "1.2.3", python: "1.2.3" });
  assert.deepEqual(versionsFromTag("v1.2.3-rc.4"), {
    tag: "v1.2.3-rc.4", npm: "1.2.3-rc.4", python: "1.2.3rc4",
  });
  for (const value of ["1.2.3", "v01.2.3", "v1.2", "v1.2.3-preview.1", "v1.2.3+dirty"])
    assert.throws(() => versionsFromTag(value), /invalid release tag/);
});

test("unsupported and non-glibc targets fail precisely", () => {
  assert.throws(() => targetForHost("win32", "x64"), /unsupported platform win32-x64/);
  assert.throws(() => targetForHost("linux", "x64", undefined), /musl\/unknown libc/);
});

test("missing optional native package names the exact dependency", () => {
  const root = mkdtempSync(join(tmpdir(), "tny-meta-"));
  try {
    writeFileSync(join(root, "package.json"), '{"name":"@thehumanworks/tny","version":"1.2.3"}\n');
    assert.throws(() => resolveNativeAddon({
      packageRoot: root, platform: "linux", architecture: "x64", glibcVersion: "2.34",
      allowCheckoutBuild: false,
      resolvePackage() { const error = new Error("not found"); error.code = "MODULE_NOT_FOUND"; throw error; },
    }), /missing optional native package @thehumanworks\/tny-linux-x64@1\.2\.3/);
  } finally { rmSync(root, { recursive: true, force: true }); }
});

test("wrong-platform metadata and tampered artifacts are rejected before loading", () => {
  const root = mkdtempSync(join(tmpdir(), "tny-native-"));
  const prebuilds = join(root, "prebuilds");
  mkdirSync(prebuilds);
  const target = targetForHost("linux", "x64", "2.34");
  const addon = join(prebuilds, "tny.node");
  const library = join(prebuilds, "libtny.so.1");
  const elf = Buffer.alloc(64); elf[0] = 0x7f; elf.write("ELF", 1); elf[4] = 2; elf[5] = 1; elf.writeUInt16LE(62, 18);
  writeFileSync(addon, elf); writeFileSync(library, elf);
  writeFileSync(join(root, "package.json"),
    '{"name":"@thehumanworks/tny-linux-x64","version":"1.2.3"}\n');
  const manifest = {
    schemaVersion: 3, packageName: target.packageName, packageVersion: "1.2.3",
    platform: "darwin", architecture: "x64", abiMajor: 1, abiMinor: 0,
    libraryVersion: "1.2.3", capabilityStructSize: 240,
    linkage: "shared-loader-relative", minimumGlibc: "2.17",
    dynamicIdentity: { kind: "soname", value: "libtny.so.1" },
    addonSha256: hash(addon), librarySha256: hash(library),
  };
  try {
    writeFileSync(join(prebuilds, "manifest.json"), `${JSON.stringify(manifest)}\n`);
    assert.throws(() => verifyNativePackage(root, target, "1.2.3"), /incompatible artifact metadata/);
    manifest.platform = "linux";
    manifest.abiMajor = 0;
    writeFileSync(join(prebuilds, "manifest.json"), `${JSON.stringify(manifest)}\n`);
    assert.throws(
      () => verifyNativePackage(root, target, "1.2.3", { glibcVersion: "2.34" }),
      /incompatible artifact metadata/,
    );
    manifest.abiMajor = 1;
    writeFileSync(join(prebuilds, "manifest.json"), `${JSON.stringify(manifest)}\n`);
    writeFileSync(addon, Buffer.concat([elf, Buffer.from("tampered")]));
    assert.throws(
      () => verifyNativePackage(root, target, "1.2.3", { glibcVersion: "2.34" }),
      /SHA-256 integrity/,
    );
  } finally { rmSync(root, { recursive: true, force: true }); }
});


test("native executor requires exact identity, hash, architecture and executable file", () => {
  const root = mkdtempSync(join(tmpdir(), "tny-executor-"));
  const target = targetForHost("linux", "x64", "2.34");
  const path = join(root, "tny-executor");
  const elf = Buffer.alloc(64);
  elf[0] = 0x7f; elf.write("ELF", 1); elf[4] = 2; elf[5] = 1; elf.writeUInt16LE(62, 18);
  const manifest = { abiMinor: 5, libraryVersion: "1.2.3" };
  try {
    assert.equal(verifyNativeExecutor(root, { abiMinor: 4 }, target), undefined);
    assert.throws(() => verifyNativeExecutor(root, manifest, target), /manifest is missing/);
    writeFileSync(path, elf, { mode: 0o755 });
    manifest.executor = { filename: "tny-executor", protocolVersion: 1, libraryVersion: "1.2.3", sha256: hash(path) };
    assert.equal(verifyNativeExecutor(root, manifest, target), path);
    for (const patch of [
      { filename: "../tny" }, { protocolVersion: 2 }, { libraryVersion: "1.2.4" }, { sha256: "bad" },
    ]) {
      assert.throws(() => verifyNativeExecutor(root, {
        ...manifest, executor: { ...manifest.executor, ...patch },
      }, target), /manifest is missing or incompatible/);
    }
    chmodSync(path, 0o644);
    assert.throws(() => verifyNativeExecutor(root, manifest, target), /regular executable/);
    chmodSync(path, 0o755);
    writeFileSync(path, Buffer.concat([elf, Buffer.from("tampered")]));
    assert.throws(() => verifyNativeExecutor(root, manifest, target), /SHA-256/);
    elf.writeUInt16LE(183, 18);
    writeFileSync(path, elf);
    manifest.executor.sha256 = hash(path);
    assert.throws(() => verifyNativeExecutor(root, manifest, target), /non-x64/);
    rmSync(path);
    assert.throws(() => verifyNativeExecutor(root, manifest, target), /executor is missing/);
    const other = join(root, "other");
    writeFileSync(other, elf, { mode: 0o755 });
    symlinkSync(other, path);
    assert.throws(() => verifyNativeExecutor(root, manifest, target), /regular executable/);
  } finally { rmSync(root, { recursive: true, force: true }); }
});
