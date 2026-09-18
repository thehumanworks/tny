// Offline 32x256KiB benchmark; optional baseline index.mjs file URL.
// Run with --expose-gc in fresh processes. V8 may retain baseline prompts as
// ropes: serialized bytes do not imply equivalent heap allocations.
import { performance } from "node:perf_hooks";

const { Workflow, WorkflowTask } = await import(process.argv[2] ?? "../dist/index.mjs");
let enter;
let release;
const entered = new Promise((resolve) => { enter = resolve; });
const barrier = new Promise((resolve) => { release = resolve; });
const original = WorkflowTask.prototype._prompt;
let renders = 0;
let promptBytes = 0;
let peakHeap = 0;
const sample = () => { peakHeap = Math.max(peakHeap, process.memoryUsage().heapUsed); };
WorkflowTask.prototype._prompt = function () {
  if (this.name !== "source") renders++;
  sample();
  return original.call(this);
};
global.gc();
const initialHeap = process.memoryUsage().heapUsed;
const started = performance.now();
const workflow = new Workflow({ maxConcurrency: 1, runner: async (task, prompt) => {
  sample();
  if (task.name === "source") return { output: "x".repeat(262144) };
  promptBytes = Buffer.byteLength(prompt);
  enter();
  await barrier;
  sample();
  return { output: "ok" };
}}).task("source", "produce");
for (let index = 0; index < 32; index++) {
  workflow.task(`consumer-${index}`, "consume", { dependsOn: ["source"] });
}
const run = workflow.run();
await entered;
await new Promise((resolve) => setImmediate(resolve));
sample();
const atBarrier = { renders, composed_bytes: renders * promptBytes,
  heap_bytes: process.memoryUsage().heapUsed, latency_ms: performance.now() - started };
release();
const result = await run;
if (!result.ok) throw new Error("fixture failed");
sample();
console.log(JSON.stringify({ variant: process.argv[2] ? "baseline" : "candidate",
  barrier: atBarrier, initial_heap_bytes: initialHeap, sampled_peak_heap_bytes: peakHeap,
  peak_rss_bytes: process.resourceUsage().maxRSS * 1024,
  total_composed_bytes: renders * promptBytes, total_ms: performance.now() - started }));
