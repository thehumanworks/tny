import {
  PermissionDecision,
  Runtime,
  type TnyEvent,
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
