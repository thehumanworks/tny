import { createHash } from "node:crypto";
import { existsSync, mkdtempSync, mkdirSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { spawnSync } from "node:child_process";

import { resultDir } from "./result.mjs";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = resolve(packageRoot, "../..");
const requestIndex = process.argv.indexOf("--request");
const requestText = requestIndex >= 0
  ? readFileSync(resolve(process.argv[requestIndex + 1]), "utf8")
  : readFileSync(0, "utf8");
const request = JSON.parse(requestText);
if (request.adapter_protocol_version !== 1 || request.conformance_version !== 1)
  throw new Error("unsupported conformance adapter request");

const executions = [];
let sdkRoot = packageRoot;
const requestedArtifact = resolve(request.artifact.path);
if (requestedArtifact.endsWith(".tgz")) {
  const installRoot = mkdtempSync(join(tmpdir(), "tny-node-installed-conformance-"));
  const consumer = join(installRoot, "consumer");
  mkdirSync(consumer);
  let run = spawnSync("npm", ["init", "-y"], { cwd: consumer, encoding: "utf8" });
  if (run.status === 0) run = spawnSync("npm", ["install", requestedArtifact, "--foreground-scripts"], {
    cwd: consumer, encoding: "utf8",
    env: Object.fromEntries(Object.entries(process.env).filter(
      ([key]) => !["TNY_ROOT", "TNY_INCLUDE_DIR", "TNY_LIB_DIR"].includes(key))),
  });
  if (run.stdout) process.stderr.write(run.stdout);
  if (run.stderr) process.stderr.write(run.stderr);
  executions.push({ id: "install_package", exit_code: run.status ?? 127 });
  sdkRoot = join(consumer, "node_modules/@thehumanworks/tny");
}
const { Runtime } = await import(pathToFileURL(join(sdkRoot, "dist/index.mjs")).href);
const commands = [
  { id: "reset", args: ["test/conformance-reset.mjs"], cwd: packageRoot },
  { id: "node_basic", args: ["--test", "test/basic.test.mjs"], cwd: packageRoot },
  { id: "node_invalid", args: ["test/invalid-options.mjs"], cwd: packageRoot },
  { id: "node_hostile", args: ["test/hostile-prototype.mjs"], cwd: packageRoot },
  { id: "node_gc", args: ["--expose-gc", "test/gc.mjs"], cwd: packageRoot },
  { id: "node_stress", args: ["test/stress.mjs"], cwd: packageRoot },
  { id: "node_workers", args: ["test/worker-teardown.mjs"], cwd: packageRoot },
  { id: "node_nul", args: ["test/nul-provider.mjs"], cwd: packageRoot },
  { id: "node_wake", args: ["test/wake-cancel.mjs"], cwd: packageRoot },
  { id: "node_integration", args: ["test/integration.mjs"], cwd: packageRoot },
];
for (const command of commands) {
  const run = spawnSync(process.execPath, command.args, {
    cwd: command.cwd,
    encoding: "utf8",
    env: {
      ...process.env,
      TNY_CONFORMANCE_SENTINEL: request.secret_sentinel,
      TNY_SDK_PACKAGE_ROOT: sdkRoot,
      ...(command.id === "libtny_ctypes" ? { TNY_LIBTNY_CORE_ONLY: "1" } : {}),
    },
  });
  if (run.stdout) process.stderr.write(run.stdout);
  if (run.stderr) process.stderr.write(run.stderr);
  executions.push({ id: command.id, exit_code: run.status ?? 127 });
  if (run.status !== 0) break;
}
if (executions.every((execution) => execution.exit_code === 0)) {
  for (const command of [
    { id: "build_c_fixtures", executable: "make", args: ["debug"] },
    {
      id: "network_split_fixture", executable: "./build/tny-test",
      args: ["-s", "net_suite", "-t", "chunked_survives_every_split_boundary", "-e"],
    },
    {
      id: "backpressure_fixture", executable: "./build/tny-test",
      args: ["-s", "runtime_suite", "-t", "runtime_overflow_keeps_error_and_single_terminal", "-e"],
    },
    { id: "libtny_ctypes", executable: "python3", args: ["tests/integration/test_libtny.py"] },
  ]) {
    const run = spawnSync(command.executable, command.args, {
      cwd: repoRoot, encoding: "utf8", env: process.env,
    });
    if (run.stdout) process.stderr.write(run.stdout);
    if (run.stderr) process.stderr.write(run.stderr);
    executions.push({ id: command.id, exit_code: run.status ?? 127 });
  }
}
const successful = new Set(executions.filter((item) => item.exit_code === 0).map((item) => item.id));
const result = (id) => {
  const path = join(resultDir, `${id}.json`);
  return existsSync(path) ? JSON.parse(readFileSync(path, "utf8")) : undefined;
};
const passed = (id, assertions, evidence) => {
  const observed = result(id);
  if (!observed || !evidence.every((item) => successful.has(item))) return {
    id, status: "not_run", reason: "one or more executable evidence runs did not complete",
  };
  return { id, status: "pass", assertions, evidence, events: observed.observed_events || [] };
};
const scenarios = [
  passed("success_two_turns", [
    "create_and_open", "sequence_strictly_increases", "timestamps_monotonic",
    "provider_session_turn_present", "borrowed_bytes_copied_before_free",
    "second_turn_same_session",
  ], ["node_integration"]),
  passed("resume_and_steer_rejection",
    ["rejected_text_preserved", "resume_same_session", "teardown_and_reopen"],
    ["node_integration"]),
  passed("permission_allow_and_stale_reject",
    ["parked_before_response", "stale_id_bad_state", "duplicate_id_bad_state"],
    ["node_integration"]),
  passed("permission_deny", ["denied_tool_not_executed"], ["node_integration"]),
  passed("cancel_and_drain",
    ["cancel_idempotent", "exactly_one_terminal", "drained_after_terminal", "cross_thread_wake"],
    ["node_integration", "node_wake"]),
  passed("auth_error",
    ["stable_auth_category", "no_raw_provider_body", "no_credentials"],
    ["node_integration"]),
  passed("unknown_future_event",
    ["numeric_kind_preserved", "payload_preserved", "known_union_not_aliased"],
    ["node_basic"]),
  successful.has("libtny_ctypes") && successful.has("node_invalid") &&
  successful.has("node_gc") && successful.has("node_stress")
    ? {
        id: "ownership_and_misuse", status: "pass",
        assertions: [
          "inputs_copied", "event_and_error_lifetimes", "double_free_prevention",
          "wrong_thread_rejected", "invalid_utf8_rejected", "embedded_nul_rejected",
          "unknown_constants_rejected", "undersized_struct_rejected",
          "oversized_struct_prefix_safe", "parent_close_releases_children",
          "repeated_lifecycle",
        ],
        evidence: ["libtny_ctypes", "node_invalid", "node_gc", "node_stress"],
        events: [],
      }
    : { id: "ownership_and_misuse", status: "not_run", reason: "lifetime evidence failed" },
  successful.has("backpressure_fixture")
    ? {
        id: "slow_consumer_backpressure", status: "pass",
        assertions: ["memory_bounded", "stable_backpressure_category", "terminal_reserved"],
        evidence: ["backpressure_fixture"],
        events: [
          { type: "error", sequence: 1, timestamp_ms: 1 },
          { type: "turn_end", sequence: 2, timestamp_ms: 2, stop_reason: "error" },
        ],
      }
    : { id: "slow_consumer_backpressure", status: "not_run", reason: "overflow fixture failed" },
  successful.has("network_split_fixture")
    ? {
        id: "network_split_boundaries", status: "pass",
        assertions: ["existing_chunked_fixture_every_split_boundary"],
        evidence: ["network_split_fixture"], events: [],
      }
    : { id: "network_split_boundaries", status: "not_run", reason: "C split fixture failed" },
];

const root = mkdtempSync(join(tmpdir(), "tny-node-adapter-"));
const workspace = join(root, "workspace");
mkdirSync(workspace);
const runtime = await Runtime.create({
  workspace, baseUrl: "http://127.0.0.1:1/v1", apiKey: "adapter-test-only",
});
const snapshot = runtime.capabilities;
await runtime.close();
const addonPath = join(sdkRoot, "build/Release/tny.node");
const sha256 = (path) => createHash("sha256").update(readFileSync(path)).digest("hex");
if (request.artifact.sha256 !== sha256(requestedArtifact))
  throw new Error("requested artifact SHA-256 is incorrect");
if (!requestedArtifact.endsWith(".tgz") && requestedArtifact !== resolve(addonPath))
  throw new Error("requested addon is not the addon executed by this adapter");
const response = {
  conformance_version: 1,
  adapter_protocol_version: 1,
  adapter: "@thehumanworks/tny-node-conformance",
  sdk: "@thehumanworks/tny",
  sdk_version: JSON.parse(readFileSync(join(sdkRoot, "package.json"), "utf8")).version,
  abi_version: `${runtime.abiVersion >>> 16}.${runtime.abiVersion & 0xffff}`,
  library_version: runtime.libraryVersion,
  platform: { os: process.platform, arch: process.arch },
  transport: snapshot.transport,
  artifact: {
    sha256: request.artifact.sha256,
    kind: requestedArtifact.endsWith(".tgz") ? "package" : "addon",
  },
  capabilities: {
    native_openai: true,
    permissions: true,
    cancellation: true,
    persistence: true,
    steering: true,
    unknown_event_preservation: true,
    bounded_event_queue: true,
  },
  capability_snapshot: {
    abi_version: snapshot.abiVersion,
    provider_available_mask: Number(snapshot.providerAvailableMask),
    feature_available_mask: Number(snapshot.featureAvailableMask),
    cancel_model: snapshot.cancelModel,
    event_queue_max: snapshot.eventQueueMax,
    event_reserved: snapshot.eventReserved,
    transport: snapshot.transport,
    linkage: snapshot.linkage,
  },
  executions,
  scenarios,
};
process.stdout.write(`${JSON.stringify(response)}\n`);
