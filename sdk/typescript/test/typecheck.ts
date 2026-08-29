import {
  PermissionDecision,
  Runtime,
  Workflow,
  WorkflowTaskExecution,
  WorkflowTaskStatus,
  type TnyEvent,
  type WorkflowDependency,
  type WorkflowResult,
  type WorkflowTaskRunner,
  type TaskPreset,
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
  const task: TaskPreset = { name: "review" };
  await using runtime = await Runtime.create({ workspace, taskPreset: task });
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
  const dependencies = ["inspect"] as const;
  const orderingOnly: WorkflowDependency = {
    name: "implement",
    includeOutput: false,
  };
  workflow.task("implement", "implement", { dependsOn: dependencies });
  workflow.task("review", "review", {
    dependsOn: [orderingOnly],
  });
  // @ts-expect-error A bare string is not a dependency list.
  workflow.task("invalid", "invalid", { dependsOn: "inspect" });
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
