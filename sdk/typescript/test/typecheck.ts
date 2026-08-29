import {
  PermissionDecision,
  Runtime,
  Workflow,
  WorkflowTaskExecution,
  WorkflowTaskStatus,
  type TnyEvent,
  type WorkflowResult,
  type WorkflowTaskRunner,
} from "@thehumanworks/tny";

declare const workspace: string;

function describe(event: TnyEvent): string {
  switch (event.type) {
    case "text_delta":
      return event.text;
    case "permission_request":
      return event.permissionId;
    case "unknown":
      return `${event.kind}:${String(event.originalType ?? "")}`;
    default:
      return event.type;
  }
}

async function useSdk(): Promise<void> {
  await using runtime = await Runtime.create({ workspace });
  await using session = await runtime.createSession();
  for await (const event of session.run("hello")) {
    describe(event);
    if (event.type === "permission_request") {
      await session.respondPermission(event.permissionId, PermissionDecision.deny);
    }
  }
}

void useSdk;


const customRunner: WorkflowTaskRunner = async (task, prompt, context) => {
  if (context.signal.aborted) throw context.signal.reason;
  return new WorkflowTaskExecution({
    output: `${task.name}:${prompt}`,
    stopReason: "done",
  });
};

async function useWorkflow(): Promise<void> {
  const workflow = new Workflow({
    runtime: { workspace },
    maxConcurrency: 2,
    onPermission: () => PermissionDecision.deny,
  });
  workflow.task("inspect", "inspect the repository");
  workflow.task("implement", "implement", { dependsOn: ["inspect"] });
  const result: WorkflowResult = await workflow.run();
  const implementation = result.require("implement");
  const status = implementation.status;
  const output: string = result.output("implement");
  if (status === WorkflowTaskStatus.failed) result.raiseForFailure();
  for (const [name, task] of result) {
    void name;
    void task.ok;
  }
  void output;

  const custom = new Workflow({ runner: customRunner }).task("one", "prompt");
  await custom.run();
}

void useWorkflow;
