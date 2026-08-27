import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { resolveNativeAddon, verifyNativePackage } from "../scripts/native-loader.mjs";
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
  const library = join(prebuilds, "libtny.so.0");
  const elf = Buffer.alloc(64); elf[0] = 0x7f; elf.write("ELF", 1); elf[4] = 2; elf[5] = 1; elf.writeUInt16LE(62, 18);
  writeFileSync(addon, elf); writeFileSync(library, elf);
  writeFileSync(join(root, "package.json"),
    '{"name":"@thehumanworks/tny-linux-x64","version":"1.2.3"}\n');
  const manifest = {
    schemaVersion: 2, packageName: target.packageName, packageVersion: "1.2.3",
    platform: "darwin", architecture: "x64", abiMajor: 0, abiMinor: 5,
    libraryVersion: "1.2.3", capabilityStructSize: 240,
    linkage: "shared-loader-relative", minimumGlibc: "2.17",
    addonSha256: hash(addon), librarySha256: hash(library),
  };
  try {
    writeFileSync(join(prebuilds, "manifest.json"), `${JSON.stringify(manifest)}\n`);
    assert.throws(() => verifyNativePackage(root, target, "1.2.3"), /incompatible artifact metadata/);
    manifest.platform = "linux";
    writeFileSync(join(prebuilds, "manifest.json"), `${JSON.stringify(manifest)}\n`);
    writeFileSync(addon, Buffer.concat([elf, Buffer.from("tampered")]));
    assert.throws(
      () => verifyNativePackage(root, target, "1.2.3", { glibcVersion: "2.34" }),
      /SHA-256 integrity/,
    );
  } finally { rmSync(root, { recursive: true, force: true }); }
});
