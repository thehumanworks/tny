import { PermissionDecision, Workflow } from "@thehumanworks/tny";

const workflow = new Workflow({
  runtime: {
    workspace: process.cwd(),
    baseUrl: process.env.OPENAI_BASE_URL ?? "https://api.openai.com/v1",
    apiKey: process.env.OPENAI_API_KEY,
    permissionMode: "ask",
  },
  maxConcurrency: 2,
  onPermission: () => PermissionDecision.deny,
});

workflow
  .task("architecture", "Audit this repository's architecture.")
  .task("tests", "Audit this repository's test coverage.")
  .task("plan", "Create one implementation plan grounded in both reports.", {
    dependsOn: ["architecture", "tests"],
  });

const result = await workflow.run();
result.raiseForFailure();
console.log(result.output("plan"));
