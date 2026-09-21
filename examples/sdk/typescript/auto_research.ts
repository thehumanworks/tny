/**
 * Auto-research: a research loop that improves within a run and across runs.
 *
 *     plan ──► investigate (parallel) ──► synthesise ──► critique ─┐
 *       ▲            ▲                                             │
 *       │            └──── gaps become the next round's questions ─┘
 *       └── lessons from earlier runs          retro ──► lessons file
 *
 * Within a run the critic's gaps drive further rounds until the score reaches
 * `--target`. Across runs a retrospective distils process lessons into a file
 * that the next run's planner reads. The Python and TypeScript versions share
 * that file and the role prompts in ../prompts.
 *
 *     OPENAI_API_KEY=... node auto_research.ts "How does session isolation work?"
 */

import { writeFileSync } from "node:fs";
import { parseArgs } from "node:util";

import { Workflow } from "@thehumanworks/tny";

import {
  AgentError,
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

const HELP = `usage: node auto_research.ts [options] TOPIC

  --rounds N          maximum rounds (default: 3)
  --target SCORE      stop once the critic scores the report at least this (default: 0.8)
  --questions N       questions per round (default: 4)
  --out FILE          write the report here instead of stdout
${RUNTIME_HELP}`;

const ID = /^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$/;

interface Question {
  id: string;
  question: string;
  approach: string;
}

interface Critique {
  score: number;
  gaps: string[];
}

function validatePlan(limit: number): (value: unknown) => Question[] {
  return (value) => {
    requireReply(isRecord(value), "reply must be a JSON object");
    const questions = value.questions;
    requireReply(Array.isArray(questions) && questions.length > 0, "questions must be a non-empty list");
    const cleaned: Question[] = [];
    for (const item of questions.slice(0, limit)) {
      requireReply(isRecord(item), "each question must be an object");
      const { id, question, approach } = item;
      requireReply(typeof id === "string" && ID.test(id), "each id must be a short slug of letters, digits, '-' or '_'");
      requireReply(typeof question === "string" && question.trim() !== "", "question must be text");
      requireReply(cleaned.every((seen) => seen.id !== id), "ids must be unique");
      cleaned.push({ id, question: question.trim(), approach: typeof approach === "string" ? approach.trim() : "" });
    }
    return cleaned;
  };
}

function validateCritique(value: unknown): Critique {
  requireReply(isRecord(value), "reply must be a JSON object");
  const score = value.score;
  requireReply(typeof score === "number" && score >= 0 && score <= 1, "score must be a number from 0 to 1");
  return { score, gaps: stringList(value.gaps ?? [], "gaps", 8) };
}

/** Fan out one agent per question; a failed branch never sinks the others. */
async function investigate(
  roles: Roles,
  topic: string,
  questions: readonly Question[],
  ledger: Ledger,
  jobs: number,
): Promise<Map<string, string>> {
  const workflow = new Workflow({
    // `auto` lets investigators read, run read-style shell and use the web;
    // anything else prompts, and the policy below denies it.
    runtime: roles.options("research-investigator", "auto"),
    maxConcurrency: jobs,
    onEvent: logEvent,
    onPermission: permissionPolicy(() => false),
  });
  for (const item of questions) {
    const approach = item.approach ? `\nSuggested approach: ${item.approach}` : "";
    workflow.task(`q-${item.id}`, `Research topic: ${topic}\n\nYour question: ${item.question}${approach}`);
  }
  const result = await workflow.run();
  ledger.addWorkflow(result);

  const findings = new Map<string, string>();
  for (const item of questions) {
    const task = result.require(`q-${item.id}`);
    if (task.ok) findings.set(item.question, task.output);
    else log(`  [${task.name}] ${task.status}: ${task.error?.message ?? ""}`);
  }
  if (findings.size === 0) throw new AgentError("every investigator failed; nothing to synthesise");
  return findings;
}

async function main(): Promise<number> {
  const { values, positionals } = parseArgs({
    allowPositionals: true,
    options: {
      rounds: { type: "string", default: "3" },
      target: { type: "string", default: "0.8" },
      questions: { type: "string", default: "4" },
      out: { type: "string" },
      ...runtimeArguments,
    },
  });
  if (values.help) {
    console.log(HELP);
    return 0;
  }
  const topic = positionals[0];
  if (topic === undefined || positionals.length !== 1) throw new UsageError(`expected exactly one TOPIC\n${HELP}`);
  const rounds = integer(values.rounds, "--rounds", 1);
  const perRound = integer(values.questions, "--questions", 1);
  const target = Number(values.target);
  if (!(target >= 0 && target <= 1)) throw new UsageError("--target must be a number from 0 to 1");

  const args = runtimeArgumentsFrom(values, ".");
  const roles = new Roles(args);
  const ledger = new Ledger();
  const lessons = Lessons.forWorkflow(args, "auto-research");
  const recalled = lessons.load();
  log(`recalled ${recalled.length} lesson(s) from ${lessons.path}`);

  let questions = await askJson(
    "plan",
    roles.options("research-planner"),
    `Research topic: ${topic}\n\nPlan at most ${perRound} questions. JSON shape:\n` +
      '{"questions": [{"id": "short-slug", "question": "...", "approach": "..."}]}' +
      lessons.promptBlock(),
    ledger,
    validatePlan(perRound),
  );

  let report = "";
  const history: { round: number; questions: string[]; answered: number; score: number; gaps: string[] }[] = [];
  for (let round = 1; round <= rounds; round += 1) {
    log(
      `round ${round}/${rounds}: investigating ${questions.length} question(s) ` +
        `[${roles.describe("research-investigator")}]`,
    );
    const findings = await investigate(roles, topic, questions, ledger, args.jobs);
    const sections = [...findings]
      .map(([question, finding]) => `<finding question="${question}">\n${finding}\n</finding>`)
      .join("\n\n");
    const previous = report ? `\n\nPrevious draft to revise:\n${report}` : "";
    report = await ask(
      "synthesise",
      roles.options("research-synthesiser"),
      `Research topic: ${topic}\n\nNew findings:\n${sections}${previous}`,
      ledger,
    );
    const critique = await askJson(
      "critique",
      roles.options("research-critic"),
      `Round ${round} of ${rounds}. Research topic: ${topic}\n\nReport:\n${report}\n\n` +
        'JSON shape: {"score": 0.0-1.0, "strengths": ["..."], "gaps": ["one ' +
        'answerable follow-up question per gap"]}',
      ledger,
      validateCritique,
    );
    history.push({
      round,
      questions: questions.map((item) => item.question),
      answered: findings.size,
      score: critique.score,
      gaps: critique.gaps,
    });
    log(`  score ${critique.score.toFixed(2)}, ${critique.gaps.length} gap(s)`);
    if (critique.score >= target || critique.gaps.length === 0) break;
    questions = critique.gaps
      .slice(0, perRound)
      .map((gap, index) => ({ id: `r${round + 1}-${index + 1}`, question: gap, approach: "" }));
  }

  const learned = await askJson(
    "retro",
    roles.options("research-retro"),
    `Research topic: ${topic}\n\nRun history:\n${JSON.stringify(history, null, 2)}` +
      `\n\nLessons already recorded:\n${JSON.stringify(recalled.slice(-20), null, 2)}\n\n` +
      'JSON shape: {"lessons": ["..."]}',
    ledger,
    validateLessons,
  );
  const added = lessons.append(learned);

  if (values.out) {
    writeFileSync(values.out, `${report}\n`);
    log(`report written to ${values.out}`);
  } else {
    console.log(report);
  }
  const scores = history.map((entry) => entry.score.toFixed(2)).join(" -> ");
  log(`scores by round: ${scores}; learned ${added.length} new lesson(s); usage: ${ledger}`);
  return 0;
}

await run(main);
