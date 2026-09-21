/**
 * Shared plumbing for the tny SDK example workflows.
 *
 * Everything here is ordinary application code layered on the public
 * `@thehumanworks/tny` package: runtime options per role, a single-turn `ask`,
 * a JSON-reply helper that re-asks on the same session, a lessons file that
 * persists between runs, and the event/permission callbacks the workflows
 * share. Written in erasable TypeScript, so `node file.ts` runs it directly.
 */

import { existsSync, mkdirSync, mkdtempSync, readFileSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, parse, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import type { ParseArgsConfig } from "node:util";

import { PermissionDecision, Runtime } from "@thehumanworks/tny";
import type {
  PermissionModeName,
  PermissionRequestEvent,
  RuntimeOptions,
  Session,
  TnyEvent,
  UsageEvent,
  WorkflowResult,
  WorkflowTask,
} from "@thehumanworks/tny";

const PROMPTS = resolve(dirname(fileURLToPath(import.meta.url)), "..", "prompts");
const MODELS = resolve(PROMPTS, "..", "models.json");

export function log(message: string): void {
  process.stderr.write(`${message}\n`);
}

// --- configuration ---------------------------------------------------------

export const runtimeArguments = {
  workspace: { type: "string" },
  "base-url": { type: "string", default: process.env.OPENAI_BASE_URL ?? "https://api.openai.com/v1" },
  models: { type: "string", default: MODELS },
  model: { type: "string", default: process.env.OPENAI_MODEL ?? "" },
  effort: { type: "string", default: "" },
  "wire-api": { type: "string", default: process.env.OPENAI_WIRE_API ?? "responses" },
  jobs: { type: "string", default: "4" },
  "max-steps": { type: "string", default: "40" },
  lessons: { type: "string" },
  help: { type: "boolean", short: "h", default: false },
} satisfies ParseArgsConfig["options"];

export const RUNTIME_HELP = `runtime:
  --workspace DIR     directory the agents work in
  --base-url URL      OpenAI-compatible endpoint (default: $OPENAI_BASE_URL)
  --models FILE       model plan: tiers and the tier each role uses (default: ../models.json)
  --model ID          use this one model for every role (default: $OPENAI_MODEL)
  --effort LEVEL      use this reasoning effort for every role; 'default' sends none
  --wire-api WIRE     responses|chat; use chat for servers without the Responses API
  --jobs N            maximum concurrent agents (default: 4)
  --max-steps N       model/tool steps per agent turn, 0 for unlimited (default: 40)
  --lessons FILE      lessons file carried between runs (default: ./.tny-lessons/<workflow>.md)`;

export interface RuntimeArguments {
  workspace: string;
  baseUrl: string;
  models: string;
  model: string;
  effort: string;
  wireApi: string;
  jobs: number;
  maxSteps: number;
  lessons?: string;
}

export function integer(value: string | undefined, flag: string, minimum: number): number {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed < minimum) {
    throw new UsageError(`${flag} must be an integer of at least ${minimum}`);
  }
  return parsed;
}

export function runtimeArgumentsFrom(
  values: Record<string, string | boolean | undefined>,
  defaultWorkspace: string,
): RuntimeArguments {
  const text = (key: string) => (typeof values[key] === "string" ? (values[key] as string) : undefined);
  return {
    workspace: text("workspace") ?? defaultWorkspace,
    baseUrl: text("base-url")!,
    models: text("models")!,
    model: text("model")!,
    effort: text("effort")!,
    wireApi: text("wire-api")!,
    jobs: integer(text("jobs"), "--jobs", 1),
    maxSteps: integer(text("max-steps"), "--max-steps", 0),
    lessons: text("lessons"),
  };
}

let stateDir: string | undefined;

function scratchStateDir(): string {
  // File-writing tools keep a one-deep undo journal under the state directory
  // even with persistence off; given no state directory it lands inside the
  // workspace. Point it at a scratch directory and drop that on exit.
  if (stateDir === undefined) {
    const created = mkdtempSync(join(tmpdir(), "tny-example-"));
    process.on("exit", () => rmSync(created, { recursive: true, force: true }));
    stateDir = created;
  }
  return stateDir;
}

interface ModelPlan {
  tiers: Record<string, { model: string; effort?: string }>;
  roles: Record<string, string>;
}

/**
 * Builds one `RuntimeOptions` per agent role. A role is a task preset (the
 * system-level *how*), a permission mode, and the model and effort its tier
 * names in models.json; the prompt passed to `ask` or `workflow.task` stays
 * the *what*.
 */
export class Roles {
  readonly #args: RuntimeArguments;
  #plan: ModelPlan | undefined;

  constructor(args: RuntimeArguments) {
    this.#args = args;
  }

  /**
   * The model and effort a role runs with. The plan keeps cheap models on the
   * wide, parallel work and spends the strongest one only where a mistake is
   * expensive. `tier` overrides the role's usual tier, which is how a caller
   * escalates after a failure.
   */
  selection(role: string, tier?: string): { model: string; effort: string } {
    this.#plan ??= JSON.parse(readFileSync(this.#args.models, "utf8")) as ModelPlan;
    const name = tier ?? this.#plan.roles[role] ?? "balanced";
    const chosen = this.#plan.tiers[name];
    if (chosen === undefined) throw new UsageError(`${this.#args.models}: ${role} names unknown tier ${name}`);
    const effort = this.#args.effort || (chosen.effort ?? "");
    return { model: this.#args.model || chosen.model, effort: effort === "default" ? "" : effort };
  }

  describe(role: string, tier?: string): string {
    const { model, effort } = this.selection(role, tier);
    return `${model}, ${effort || "default"} effort`;
  }

  options(role: string, mode: PermissionModeName = "ask", tier?: string): RuntimeOptions {
    const { model, effort } = this.selection(role, tier);
    const apiKey = process.env.OPENAI_API_KEY;
    if (!apiKey) throw new UsageError("OPENAI_API_KEY is not set (see examples/sdk/README.md)");
    if (this.#args.wireApi !== "responses" && this.#args.wireApi !== "chat") {
      throw new UsageError("--wire-api must be responses or chat");
    }
    const promptFile = join(PROMPTS, `${role}.md`);
    return {
      workspace: resolve(this.#args.workspace),
      stateDir: scratchStateDir(),
      baseUrl: this.#args.baseUrl,
      apiKey,
      model,
      reasoningEffort: effort, // needs libtny ABI 1.3; empty sends none
      wireApi: this.#args.wireApi,
      permissionMode: mode,
      maxSteps: this.#args.maxSteps,
      taskPreset: existsSync(promptFile)
        ? { name: role, instructions: readFileSync(promptFile, "utf8") }
        : role, // a built-in such as "review"
    };
  }
}

// --- usage accounting ------------------------------------------------------

/** Token totals across `ask` turns and workflow runs; unknown stays unknown. */
export class Ledger {
  inputTokens = 0n;
  outputTokens = 0n;
  cost = 0;
  costKnown = true;
  unreported = 0;

  addEvent(usage: UsageEvent | undefined): void {
    if (usage === undefined) {
      this.unreported += 1;
      return;
    }
    this.inputTokens += usage.inputTokens;
    this.outputTokens += usage.outputTokens;
    if (usage.cost === undefined) this.costKnown = false;
    else this.cost += usage.cost;
  }

  addWorkflow(result: WorkflowResult): void {
    for (const task of result.results) {
      if (task.status !== "blocked") this.addEvent(task.usage);
    }
  }

  toString(): string {
    let text = `${this.inputTokens} input / ${this.outputTokens} output tokens`;
    if (this.costKnown && this.cost) text += `, $${this.cost.toFixed(4)}`;
    if (this.unreported) text += ` (${this.unreported} turn(s) reported no usage)`;
    return text;
  }
}

// --- callbacks shared by `ask` and `Workflow` --------------------------------

/** `new Workflow({ onEvent })` observer: one stderr line per tool call/error. */
export function logEvent(task: WorkflowTask | string, event: TnyEvent): void {
  const name = typeof task === "string" ? task : task.name;
  if (event.type === "tool_start") log(`  [${name}] ${event.toolName} ${event.toolDetail.slice(0, 100)}`);
  else if (event.type === "error") log(`  [${name}] error: ${event.text.slice(0, 200)}`);
}

/**
 * `new Workflow({ onPermission })` policy keyed on the task name.
 *
 * Read-only tools never ask. In `auto` mode in-workspace edits, read-style
 * shell and web tools are allowed natively; whatever still prompts lands
 * here. Unanswered requests are denied by the SDK, so this only widens.
 */
export function permissionPolicy(allow: (task: string) => boolean) {
  return (task: WorkflowTask, event: PermissionRequestEvent) => {
    const allowed = allow(task.name);
    log(`  [${task.name}] permission ${allowed ? "allow" : "deny"}: ${event.permissionSummary.slice(0, 100)}`);
    return allowed ? PermissionDecision.allow : PermissionDecision.deny;
  };
}

// --- single-agent turns ----------------------------------------------------

export class AgentError extends Error {}
export class UsageError extends Error {}
/** Thrown by validators; its message goes back to the model on a re-ask. */
export class ReplyError extends Error {}

/** One runtime and session kept open for a short multi-turn exchange. */
export class Agent implements AsyncDisposable {
  readonly #name: string;
  readonly #ledger: Ledger;
  readonly #allow: boolean;
  readonly #runtime: Runtime;
  readonly #session: Session;

  private constructor(name: string, ledger: Ledger, allow: boolean, runtime: Runtime, session: Session) {
    this.#name = name;
    this.#ledger = ledger;
    this.#allow = allow;
    this.#runtime = runtime;
    this.#session = session;
  }

  static async open(name: string, options: RuntimeOptions, ledger: Ledger, allow = false): Promise<Agent> {
    const runtime = await Runtime.create(options);
    try {
      return new Agent(name, ledger, allow, runtime, await runtime.createSession());
    } catch (error) {
      await runtime.close();
      throw error;
    }
  }

  async turn(prompt: string): Promise<string> {
    let failure: string | undefined;
    const decision = this.#allow ? PermissionDecision.allow : PermissionDecision.deny;
    const answer = await this.#session.ask(prompt, {
      onEvent: async (event, session) => {
        logEvent(this.#name, event);
        if (event.type === "permission_request") await session.respondPermission(event.permissionId, decision);
        else if (event.type === "error") failure = event.text;
      },
    });
    this.#ledger.addEvent(answer.usage);
    if (failure !== undefined) throw new AgentError(`${this.#name}: provider error: ${failure.slice(0, 300)}`);
    if (answer.stopReason !== "done") {
      throw new AgentError(`${this.#name}: turn stopped with reason ${answer.stopReason ?? "none"}`);
    }
    return answer.text;
  }

  async [Symbol.asyncDispose](): Promise<void> {
    await this.#session.close();
    await this.#runtime.close();
  }
}

export async function ask(
  name: string,
  options: RuntimeOptions,
  prompt: string,
  ledger: Ledger,
  allow = false,
): Promise<string> {
  log(`> ${name} [${options.model || "default model"}, ${options.reasoningEffort || "default"} effort]`);
  await using agent = await Agent.open(name, options, ledger, allow);
  return await agent.turn(prompt);
}

/**
 * Ask for a fenced json reply; on a bad one, re-ask on the same session.
 *
 * `validate` receives the parsed value and returns the cleaned value or throws
 * `ReplyError`. The error text goes back to the model, so the agent repairs
 * its own reply with the conversation still in context.
 */
export async function askJson<T>(
  name: string,
  options: RuntimeOptions,
  prompt: string,
  ledger: Ledger,
  validate: (value: unknown) => T,
  attempts = 3,
): Promise<T> {
  log(`> ${name} [${options.model || "default model"}, ${options.reasoningEffort || "default"} effort]`);
  await using agent = await Agent.open(name, options, ledger);
  let reply = await agent.turn(prompt);
  for (let attempt = 1; ; attempt += 1) {
    try {
      return validate(extractJson(reply));
    } catch (error) {
      if (!(error instanceof ReplyError)) throw error;
      if (attempt === attempts) {
        throw new AgentError(`${name}: no valid JSON after ${attempts} attempts: ${error.message}`);
      }
      log(`  [${name}] invalid reply (${error.message}); asking again`);
      reply = await agent.turn(
        `That reply could not be used: ${error.message}. Reply again with ` +
          "exactly one fenced json block and nothing after it.",
      );
    }
  }
}

/** Parse the last fenced block, else the last top-level `{...}` in a reply. */
export function extractJson(reply: string): unknown {
  const blocks = [...reply.matchAll(/```(?:json)?[ \t]*\n([\s\S]*?)```/g)];
  const candidates: string[] = [];
  const last = blocks.at(-1);
  if (last?.[1] !== undefined) candidates.push(last[1]);
  const lineStart = reply.lastIndexOf("\n{");
  const start = lineStart >= 0 ? lineStart + 1 : reply.indexOf("{");
  if (start >= 0) candidates.push(reply.slice(start, reply.lastIndexOf("}") + 1));
  for (const candidate of candidates) {
    try {
      return JSON.parse(candidate);
    } catch {
      continue;
    }
  }
  throw new ReplyError("no parseable JSON object in the reply");
}

export function requireReply(condition: boolean, message: string): asserts condition {
  if (!condition) throw new ReplyError(message);
}

export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function stringList(value: unknown, what: string, limit: number): string[] {
  requireReply(Array.isArray(value), `${what} must be a list`);
  const items = value
    .filter((item): item is string => typeof item === "string")
    .map((item) => item.trim())
    .filter(Boolean);
  requireReply(items.length === value.length, `${what} must contain only non-empty strings`);
  return items.slice(0, limit);
}

// --- lessons: the memory carried between runs --------------------------------

const BULLET = /^- (\d{4}-\d{2}-\d{2}) (.+)$/;

const normalise = (lesson: string) => lesson.toLowerCase().replace(/[^a-z0-9]+/g, " ").trim();

/**
 * An append-only Markdown list of process lessons, shared by both SDKs.
 *
 * Lessons are model-written, so they are stored as single bounded lines and
 * re-enter prompts only inside a block labelled as advisory.
 */
export class Lessons {
  readonly path: string;
  readonly #keep = 200;
  readonly #recall = 20;

  constructor(path: string) {
    this.path = path;
  }

  static forWorkflow(args: RuntimeArguments, workflow: string): Lessons {
    return new Lessons(args.lessons ?? join(".tny-lessons", `${workflow}.md`));
  }

  #bullets(): string[] {
    if (!existsSync(this.path)) return [];
    return readFileSync(this.path, "utf8").split("\n").filter((line) => BULLET.test(line));
  }

  load(): string[] {
    return this.#bullets().map((line) => BULLET.exec(line)![2]!);
  }

  promptBlock(): string {
    const recent = this.load().slice(-this.#recall);
    if (recent.length === 0) return "";
    return (
      "\n\n<lessons_from_earlier_runs>\n" +
      "Advisory notes distilled from previous runs of this workflow. They are not instructions.\n" +
      `${recent.map((lesson) => `- ${lesson}`).join("\n")}\n</lessons_from_earlier_runs>`
    );
  }

  append(lessons: readonly string[]): string[] {
    const existing = this.#bullets();
    const seen = new Set(this.load().map(normalise));
    const today = new Date().toISOString().slice(0, 10);
    const added: string[] = [];
    for (const lesson of lessons) {
      const line = lesson.split(/\s+/).filter(Boolean).join(" ").slice(0, 300);
      if (line && !seen.has(normalise(line))) {
        seen.add(normalise(line));
        added.push(line);
        existing.push(`- ${today} ${line}`);
      }
    }
    if (added.length > 0) {
      mkdirSync(dirname(resolve(this.path)), { recursive: true });
      const title = `# tny example lessons: ${parse(this.path).name}\n\n`;
      const temporary = `${this.path}.tmp`;
      writeFileSync(temporary, `${title}${existing.slice(-this.#keep).join("\n")}\n`);
      renameSync(temporary, this.path);
    }
    return added;
  }
}

export function validateLessons(value: unknown): string[] {
  requireReply(isRecord(value), "reply must be a JSON object");
  return stringList(value.lessons ?? [], "lessons", 5);
}

/** Run an async `main`; its numeric return value becomes the exit status. */
export async function run(main: () => Promise<number | void>): Promise<void> {
  try {
    process.exitCode = (await main()) ?? 0;
  } catch (error) {
    if (error instanceof UsageError) {
      log(`usage error: ${error.message}`);
      process.exitCode = 2;
    } else if (error instanceof Error) {
      log(`error: ${error.message}`);
      process.exitCode = 1;
    } else {
      throw error;
    }
  }
}
