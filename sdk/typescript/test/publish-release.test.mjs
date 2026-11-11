import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import test from "node:test";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { publishArguments, publishRelease } from "../../../scripts/publish_npm_release.mjs";

const names = [
  "@nothumanwork/tny-darwin-arm64",
  "@nothumanwork/tny-linux-arm64",
  "@nothumanwork/tny-linux-x64",
  "@nothumanwork/tny",
];
const artifacts = names.map((name) => ({
  name, version: "1.2.3",
  path: `/artifacts/${name.replaceAll("/", "-")}.tgz`,
  integrity: `sha512-${createHash("sha512").update(name).digest("base64")}`,
}));
const metadata = (artifact, integrity = artifact.integrity) => ({
  name: artifact.name, version: artifact.version, dist: { integrity },
});

test("rerun with identical registry artifacts skips publishing and reads back once", async () => {
  const published = [];
  const readbacks = [];
  const statuses = await publishRelease({
    artifacts,
    lookup: async (artifact) => metadata(artifact),
    publish: async (artifact) => published.push(artifact.name),
    readback: async (ordered) => readbacks.push(ordered.map((item) => item.name)),
  });
  assert.deepEqual(published, []);
  assert.deepEqual(readbacks, [names]);
  assert.ok(statuses.every((item) => item.status === "already-present-identical"));
});

test("registry digest mismatch fails before publishing or readback", async () => {
  const actions = [];
  await assert.rejects(() => publishRelease({
    artifacts,
    lookup: async (artifact) => metadata(artifact, "sha512-wrong"),
    publish: async () => actions.push("publish"),
    readback: async () => actions.push("readback"),
  }), /registry mismatch/);
  assert.deepEqual(actions, []);
});

test("partial native publish failure is fail-fast and meta is never attempted", async () => {
  const actions = [];
  const present = new Map();
  await assert.rejects(() => publishRelease({
    artifacts,
    lookup: async (artifact) => present.get(artifact.name),
    publish: async (artifact) => {
      actions.push(artifact.name);
      if (artifact.name === "@nothumanwork/tny-linux-arm64") throw new Error("injected publish failure");
      present.set(artifact.name, metadata(artifact));
    },
    readback: async () => actions.push("readback"),
  }), /injected publish failure/);
  assert.deepEqual(actions, names.slice(0, 2));
  assert.ok(!actions.includes("@nothumanwork/tny"));
});

test("all natives are verified in sorted order before the meta package", async () => {
  const actions = [];
  const present = new Map();
  await publishRelease({
    artifacts: [...artifacts].reverse(),
    lookup: async (artifact, options) => {
      actions.push(`lookup:${artifact.name}:${options?.afterPublish ? "after" : "before"}`);
      return present.get(artifact.name);
    },
    publish: async (artifact) => {
      actions.push(`publish:${artifact.name}`);
      present.set(artifact.name, metadata(artifact));
    },
    readback: async () => actions.push("readback"),
  });
  const publishes = actions.filter((item) => item.startsWith("publish:") || item === "readback");
  assert.deepEqual(publishes, [...names.map((name) => `publish:${name}`), "readback"]);
});

test("local login publishing disables provenance but keeps public access", () => {
  const args = publishArguments("https://registry.npmjs.org", artifacts[0], { local: true });
  assert.deepEqual(args, ["publish", "--provenance=false", "--access", "public",
    "--registry", "https://registry.npmjs.org", artifacts[0].path]);
  assert.ok(publishArguments("https://registry.npmjs.org", artifacts[0]).includes("--provenance"));
});

test("publisher help and invalid arguments never need registry access", () => {
  const script = fileURLToPath(new URL("../../../scripts/publish_npm_release.mjs", import.meta.url));
  const help = spawnSync(process.execPath, [script, "--local", "--help"], { encoding: "utf8" });
  assert.equal(help.status, 0);
  assert.match(help.stdout, /--dry-run/);
  for (const args of [[], ["--root"], ["--typo"], ["--root", ".", "--version", "latest"]]) {
    const result = spawnSync(process.execPath, [script, "--local", ...args], { encoding: "utf8" });
    assert.equal(result.status, 1);
    assert.match(result.stderr, /required:|missing value|unknown option|version must/);
  }
});
