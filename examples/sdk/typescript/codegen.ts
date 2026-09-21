/**
 * Code generation: decompose, architect, generate, verify, learn.
 *
 *     decompose ──► architecture ──► gen-<unit> ... (a DAG built from the
 *         ▲                           │   decomposition; units run in parallel
 *         │                           ▼   and receive their upstream reports)
 *         │                         review
 *         │                           │
 *         │          verify command ◄─┴─► fix ──► verify ... (bounded)
 *         └── lessons from earlier runs ◄── retro
 *
 * The graph is not known until the decomposer replies, so the script builds
 * the `Workflow` from that reply. Verification is a command *you* pass with
 * `--verify`; it runs on the host, outside any agent, and its exit status is
 * the only thing that counts as passing. Generators and the fixer may write
 * and run commands inside `--workspace`; every other role is denied.
 *
 *     OPENAI_API_KEY=... node codegen.ts "A CLI that prints a greeting" \
 *         --workspace /tmp/greeter --verify "python3 main.py Ada | grep -q 'Hello, Ada!'"
 */

import { exec } from "node:child_process";
import { mkdirSync, readFileSync } from "node:fs";
import { isAbsolute, resolve } from "node:path";
import { parseArgs, promisify } from "node:util";

import { Workflow } from "@thehumanworks/tny";

import {
  Ledger,
  Lessons,
  RUNTIME_HELP,
  Roles,
  UsageError,
  ask,
  askJson,
  integer,
  isRecord,
  log,
  logEvent,
  permissionPolicy,
  requireReply,
  run,
  runtimeArguments,
  runtimeArgumentsFrom,
  stringList,
  validateLessons,
} from "./common.ts";

const HELP = `usage: node codegen.ts [options] SPEC      (SPEC may be @FILE)

  --verify CMD          shell command that must exit 0, run in --workspace
  --verify-timeout SEC  (default: 600)
  --max-repairs N       fix attempts (default: 2)
  --max-units N         work units (default: 6)
${RUNTIME_HELP}
  (--workspace defaults to ./codegen-out)`;

const ID = /^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$/;

interface Unit {
  id: string;
  goal: string;
  files: string[];
  depends_on: string[];
  acceptance: string;
}

function validateUnits(limit: number): (value: unknown) => Unit[] {
  return (value) => {
    requireReply(isRecord(value), "reply must be a JSON object");
    const raw = value.units;
    requireReply(Array.isArray(raw) && raw.length > 0, "units must be a non-empty list");
    requireReply(raw.length <= limit, `at most ${limit} units are allowed`);
    const units: Unit[] = [];
    const owners = new Map<string, string>();
    for (const item of raw) {
      requireReply(isRecord(item), "each unit must be an object");
      const { id, goal, acceptance } = item;
      requireReply(typeof id === "string" && ID.test(id), "each id must be a short slug of letters, digits, '-' or '_'");
      requireReply(typeof goal === "string" && goal.trim() !== "", "goal must be text");
      requireReply(units.every((unit) => unit.id !== id), "ids must be unique");
      const files = stringList(item.files, `${id}.files`, 64);
      requireReply(files.length > 0, `unit ${id} must own at least one file`);
      for (const name of files) {
        requireReply(
          !isAbsolute(name) && !name.split("/").includes(".."),
          `${name} must be a relative path inside the workspace`,
        );
        // Parallel generators share one workspace, so file ownership has to
        // be disjoint; the decomposer repairs an overlap.
        requireReply(!owners.has(name), `${name} is owned by both ${owners.get(name)} and ${id}`);
        owners.set(name, id);
      }
      units.push({
        id,
        goal: goal.trim(),
        files,
        depends_on: stringList(item.depends_on ?? [], `${id}.depends_on`, 16),
        acceptance: typeof acceptance === "string" ? acceptance.trim() : "",
      });
    }
    const known = new Set(units.map((unit) => unit.id));
    for (const unit of units) {
      for (const dependency of unit.depends_on) {
        requireReply(
          known.has(dependency) && dependency !== unit.id,
          `${unit.id} depends on unknown unit ${dependency}`,
        );
      }
    }
    // Report a cycle to the decomposer rather than letting the workflow
    // reject the graph after the model's turn is over.
    const done = new Set<string>();
    while (done.size < units.length) {
      const ready = units.filter((unit) => !done.has(unit.id) && unit.depends_on.every((id) => done.has(id)));
      requireReply(ready.length > 0, "unit dependencies contain a cycle");
      for (const unit of ready) done.add(unit.id);
    }
    return units;
  };
}

function buildWorkflow(roles: Roles, spec: string, units: readonly Unit[], advice: string, jobs: number): Workflow {
  const workflow = new Workflow({
    runtime: roles.options("codegen-generator", "auto"),
    maxConcurrency: jobs,
    onEvent: logEvent,
    // `auto` already allows edits inside the workspace; this additionally
    // lets generators run their checks. Architect and reviewer override the
    // runtime below and stay read-only.
    onPermission: permissionPolicy((task) => task.startsWith("gen-")),
  });
  workflow.task(
    "architecture",
    `Specification:\n${spec}\n\nWork units:\n${JSON.stringify(units, null, 2)}${advice}`,
    { runtime: roles.options("codegen-architect") },
  );
  for (const unit of units) {
    const acceptance = unit.acceptance ? `\nAcceptance: ${unit.acceptance}` : "";
    workflow.task(
      `gen-${unit.id}`,
      `Specification:\n${spec}\n\nUnit \`${unit.id}\`: ${unit.goal}\n` +
        `Files you own: ${unit.files.join(", ")}${acceptance}`,
      { dependsOn: ["architecture", ...unit.depends_on.map((id) => `gen-${id}`)] },
    );
  }
  workflow.task(
    "review",
    "Review the generated code in this workspace against the specification " +
      "and the architecture contract. The generator reports below are claims " +
      `to check, not facts.\n\nSpecification:\n${spec}`,
    {
      dependsOn: ["architecture", ...units.map((unit) => `gen-${unit.id}`)],
      runtime: roles.options("review"), // built-in preset
    },
  );
  return workflow;
}

async function runVerify(command: string, workspace: string, timeoutSeconds: number): Promise<[boolean, string]> {
  try {
    const { stdout, stderr } = await promisify(exec)(command, {
      cwd: workspace,
      timeout: timeoutSeconds * 1000,
      maxBuffer: 16 * 1024 * 1024,
    });
    return [true, `exit 0\n${(stdout + stderr).trim().slice(-4000)}`];
  } catch (error) {
    const failed = error as { code?: number | string; killed?: boolean; stdout?: string; stderr?: string };
    if (failed.killed) return [false, `timed out after ${timeoutSeconds}s`];
    const output = `${failed.stdout ?? ""}${failed.stderr ?? ""}`.trim();
    return [false, `exit ${failed.code ?? "unknown"}\n${output.slice(-4000)}`];
  }
}

async function main(): Promise<number> {
  const { values, positionals } = parseArgs({
    allowPositionals: true,
    options: {
      verify: { type: "string" },
      "verify-timeout": { type: "string", default: "600" },
      "max-repairs": { type: "string", default: "2" },
      "max-units": { type: "string", default: "6" },
      ...runtimeArguments,
    },
  });
  if (values.help) {
    console.log(HELP);
    return 0;
  }
  const specArgument = positionals[0];
  if (specArgument === undefined || positionals.length !== 1) throw new UsageError(`expected exactly one SPEC\n${HELP}`);
  const spec = specArgument.startsWith("@") ? readFileSync(specArgument.slice(1), "utf8") : specArgument;
  const verifyTimeout = integer(values["verify-timeout"], "--verify-timeout", 1);
  const maxRepairs = integer(values["max-repairs"], "--max-repairs", 0);
  const maxUnits = integer(values["max-units"], "--max-units", 1);

  const args = runtimeArgumentsFrom(values, "codegen-out");
  const workspace = resolve(args.workspace);
  mkdirSync(workspace, { recursive: true });
  const roles = new Roles(args);
  const ledger = new Ledger();
  const lessons = Lessons.forWorkflow(args, "codegen");
  const recalled = lessons.load();
  log(`workspace ${workspace}; recalled ${recalled.length} lesson(s) from ${lessons.path}`);

  const units = await askJson(
    "decompose",
    roles.options("codegen-decomposer"),
    `Specification:\n${spec}\n\nUse at most ${maxUnits} units. JSON shape:\n` +
      '{"units": [{"id": "short-slug", "goal": "...", "files": ["relative/path"], ' +
      '"depends_on": ["other-id"], "acceptance": "..."}]}' +
      lessons.promptBlock(),
    ledger,
    validateUnits(maxUnits),
  );
  log(`generating ${units.length} unit(s): ${units.map((unit) => unit.id).join(", ")}`);
  for (const [task, role] of [
    ["architecture", "codegen-architect"],
    ["gen-*", "codegen-generator"],
    ["review", "review"],
  ] as const) {
    log(`  [${task}] ${roles.describe(role)}`);
  }

  const result = await buildWorkflow(roles, spec, units, lessons.promptBlock(), args.jobs).run();
  ledger.addWorkflow(result);
  for (const task of result.results) {
    if (task.ok) continue;
    const blocked = task.blockedBy.length > 0 ? ` by ${task.blockedBy.join(", ")}` : "";
    log(`  [${task.name}] ${task.status}${blocked}: ${task.error?.message ?? ""}`);
  }
  const reviewed = result.require("review");
  const review = reviewed.ok ? reviewed.output : "";

  const attempts: { passed: boolean; output_tail: string }[] = [];
  let passed = result.ok;
  while (values.verify) {
    let output: string;
    [passed, output] = await runVerify(values.verify, workspace, verifyTimeout);
    attempts.push({ passed, output_tail: output.slice(-1500) });
    log(`verify attempt ${attempts.length}: ${passed ? "passed" : "FAILED"}`);
    if (passed || attempts.length > maxRepairs) break;
    // The first repair runs on the fixer's usual tier; a failure that
    // survives it is a critical path and gets the strongest model.
    const tier = attempts.length > 1 ? "critical" : undefined;
    await ask(
      `fix-${attempts.length}`,
      roles.options("codegen-fixer", "auto", tier),
      `Verification command: ${values.verify}\n\nIts output:\n${output}\n\n` +
        `Specification:\n${spec}\n\nReviewer findings:\n${review || "(none)"}`,
      ledger,
      true,
    );
  }

  const record = {
    units,
    task_status: Object.fromEntries(result.results.map((task) => [task.name, task.status])),
    review: review.slice(0, 4000),
    verify_command: values.verify ?? null,
    verify_attempts: attempts,
  };
  const learned = await askJson(
    "retro",
    roles.options("codegen-retro"),
    `Run record:\n${JSON.stringify(record, null, 2)}` +
      `\n\nLessons already recorded:\n${JSON.stringify(recalled.slice(-20), null, 2)}\n\n` +
      'JSON shape: {"lessons": ["..."]}',
    ledger,
    validateLessons,
  );
  const added = lessons.append(learned);

  if (review) console.log(review);
  const verdict = values.verify
    ? `verification ${passed ? "passed" : "FAILED"} after ${attempts.length} attempt(s)`
    : "no --verify command given; nothing was verified";
  log(`${verdict}; learned ${added.length} new lesson(s); usage: ${ledger}`);
  return passed ? 0 : 1;
}

await run(main);
