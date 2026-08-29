export declare const EVENT_SCHEMA_VERSION: 1;
export declare const eventKinds: Readonly<{
  text_delta: 0; thinking: 1; tool_start: 2; tool_end: 3;
  permission_request: 4; plan: 5; usage: 6; turn_end: 7; error: 8;
  status: 9; steer_rejected: 10; custom_message: 11; user_message: 12;
  tool_progress: 13;
}>;

export type PermissionModeName = "ask" | "auto" | "yolo";
export type PermissionDecisionName = "allow" | "allowAlways" | "deny";
export declare const PermissionMode: Readonly<{ ask: 0; auto: 1; yolo: 2 }>;
export declare const PermissionDecision: Readonly<{ allow: 0; allowAlways: 1; deny: 2 }>;
export declare const PermissionOption: Readonly<{ allow: 1; allowAlways: 2; deny: 4 }>;

export class TnyError extends Error {
  readonly status: number;
  constructor(message: string, status: number, options?: ErrorOptions);
}
export class UnsupportedFeatureError extends TnyError {
  readonly feature: string;
  constructor(feature: string);
}

export interface TnyEventEnvelope {
  schemaVersion: number;
  sequence: bigint;
  timestampMs: bigint;
  provider: string;
  sessionId: string;
  turnId: string;
}
export type TnyStopReason = "done" | "interrupted" | "denied" | "step_limit" | "error" | "unknown";
export interface TextDeltaEvent extends TnyEventEnvelope { type: "text_delta"; kind: 0; text: string; messageId?: string }
export interface ThinkingEvent extends TnyEventEnvelope { type: "thinking"; kind: 1; text: string; messageId?: string }
export interface ToolStartEvent extends TnyEventEnvelope { type: "tool_start"; kind: 2; toolName: string; toolId: string; toolDetail: string }
export interface ToolEndEvent extends TnyEventEnvelope { type: "tool_end"; kind: 3; toolName: string; toolId: string; toolDetail: string; toolOk: boolean }
export interface PermissionRequestEvent extends TnyEventEnvelope { type: "permission_request"; kind: 4; permissionId: string; permissionSummary: string; permissionOptions: number }
export interface PlanEvent extends TnyEventEnvelope { type: "plan"; kind: 5; text: string; messageId?: string }
export interface UsageEvent extends TnyEventEnvelope { type: "usage"; kind: 6; inputTokens: bigint; outputTokens: bigint; contextUsed: bigint; contextSize: bigint; cost?: number; hasCost: boolean }
export interface TurnEndEvent extends TnyEventEnvelope { type: "turn_end"; kind: 7; stopReason: TnyStopReason }
export interface ErrorEvent extends TnyEventEnvelope { type: "error"; kind: 8; text: string; errorCode: number }
export interface StatusEvent extends TnyEventEnvelope { type: "status"; kind: 9; text: string; messageId?: string }
export interface SteerRejectedEvent extends TnyEventEnvelope { type: "steer_rejected"; kind: 10; text: string; messageId?: string }
export interface CustomMessageEvent extends TnyEventEnvelope { type: "custom_message"; kind: 11; text: string; messageId?: string; messageType?: string }
export interface UserMessageEvent extends TnyEventEnvelope { type: "user_message"; kind: 12; text: string; messageId?: string }
export interface ToolProgressEvent extends TnyEventEnvelope { type: "tool_progress"; kind: 13; toolName: string; toolId: string; toolDetail: string }
export interface UnknownEvent extends TnyEventEnvelope {
  type: "unknown";
  kind: number;
  originalType?: string;
  payload: Readonly<Record<string, unknown>>;
}
export type KnownTnyEvent = TextDeltaEvent | ThinkingEvent | ToolStartEvent |
  ToolEndEvent | PermissionRequestEvent | PlanEvent | UsageEvent | TurnEndEvent |
  ErrorEvent | StatusEvent | SteerRejectedEvent | CustomMessageEvent |
  UserMessageEvent | ToolProgressEvent;
export type TnyEvent = KnownTnyEvent | UnknownEvent;

export interface RuntimeCapabilities {
  readonly schemaVersion: number;
  readonly abiVersion: number;
  readonly providerSelected: number;
  readonly providerInitialized: boolean;
  readonly endpointReachability: 0 | 1 | 2 | number;
  readonly threadingModel: number;
  readonly cancelModel: number;
  readonly providerAvailableMask: bigint;
  readonly featureAvailableMask: bigint;
  readonly featureEnabledMask: bigint;
  readonly eventQueueMax: number;
  readonly eventReserved: number;
  readonly eventPayloadBytesMax: bigint;
  readonly eventReservedBytes: bigint;
  readonly libraryVersion: string;
  readonly platformFamily: string;
  readonly architecture: string;
  readonly transport: string;
  readonly tlsImplementation: string;
  readonly linkage: string;
  readonly abiMajor: 1;
  readonly abiMinor: number;
  /** True only when this runtime selected an ABI 1.1 task preset. */
  readonly taskPresets: boolean;
  readonly experimental: false;
}
export interface RuntimeOptions {
  workspace: string;
  /** Required iff persistence is true. */
  stateDir?: string;
  provider?: "openai";
  model?: string;
  baseUrl?: string;
  apiKey?: string;
  wireApi?: "responses" | "chat";
  permissionMode?: PermissionModeName | 0 | 1 | 2;
  persistence?: boolean;
  /** Integer from 0 through 2,147,483,647. */
  maxSteps?: number;
  maxToolResultBytes?: number | bigint;
  /** Explicit deterministic task preset. A name selects a built-in; the
   * object form supplies a rebuild-free custom instruction body. */
  taskPreset?: string | TaskPreset;
}
export interface TaskPreset {
  readonly name: string;
  readonly instructions?: string;
}
export interface RunOptions {
  signal?: AbortSignal;
  /** Reserved for a future send_ex ABI. Currently throws UnsupportedFeatureError. */
  images?: readonly unknown[];
  /** Reserved for a future send_ex ABI. Currently throws UnsupportedFeatureError. */
  outputSchema?: Readonly<Record<string, unknown>>;
}
export interface AskOptions extends RunOptions {
  onEvent?: (event: TnyEvent, session: Session) => void | Promise<void>;
}
export interface AskResult {
  readonly text: string;
  readonly stopReason?: TnyStopReason;
  readonly usage?: UsageEvent;
}

export class Runtime implements AsyncDisposable {
  readonly abiVersion: number;
  readonly libraryVersion: string;
  readonly capabilities: RuntimeCapabilities;
  readonly closed: boolean;
  static create(options: RuntimeOptions): Promise<Runtime>;
  createSession(): Promise<Session>;
  openSession(sessionId: string): Promise<Session>;
  getCapabilities(): Promise<RuntimeCapabilities>;
  close(): Promise<void>;
  [Symbol.asyncDispose](): Promise<void>;
}
export class Session implements AsyncDisposable {
  readonly id: string;
  readonly closed: boolean;
  run(prompt: string, options?: RunOptions): AsyncGenerator<TnyEvent, void, void>;
  ask(prompt: string, options?: AskOptions): Promise<AskResult>;
  respondPermission(requestId: string, decision: PermissionDecisionName | 0 | 1 | 2): Promise<void>;
  steer(text: string): Promise<void>;
  cancel(): Promise<void>;
  close(): Promise<void>;
  [Symbol.asyncDispose](): Promise<void>;
}

export type WorkflowTaskStatusName = "success" | "failed" | "blocked";
export declare const WorkflowTaskStatus: Readonly<{
  success: "success";
  failed: "failed";
  blocked: "blocked";
}>;

export class WorkflowError extends Error {
  constructor(message: string, options?: ErrorOptions);
}
export class WorkflowDefinitionError extends WorkflowError {
  constructor(message: string, options?: ErrorOptions);
}
export class WorkflowContextError extends WorkflowError {
  constructor(message: string, options?: ErrorOptions);
}
export class WorkflowRunError extends WorkflowError {
  constructor(message: string, options?: ErrorOptions);
}

export interface WorkflowDependency {
  readonly name: string;
  /** Append this dependency's output to the consumer prompt. Defaults to true. */
  readonly includeOutput?: boolean;
}

export interface WorkflowTaskOptions {
  dependsOn?: readonly (string | WorkflowDependency)[];
  /** Override the workflow's default runtime for this task. */
  runtime?: RuntimeOptions;
}

export class WorkflowTask {
  readonly name: string;
  readonly dependsOn: readonly WorkflowDependency[];
  constructor(name: string, prompt: string, options?: WorkflowTaskOptions);
  toJSON(): {
    name: string;
    dependsOn: readonly WorkflowDependency[];
  };
}

export interface WorkflowTaskExecutionOptions {
  output: string;
  sessionId?: string;
  stopReason?: TnyStopReason;
  error?: Error;
}

export class WorkflowTaskExecution {
  readonly output: string;
  readonly sessionId: string;
  readonly stopReason?: TnyStopReason;
  readonly error?: Error;
  constructor(options: WorkflowTaskExecutionOptions);
  toJSON(): {
    outputBytes: number;
    sessionIdBytes: number;
    stopReason?: TnyStopReason;
    error?: string;
  };
}

export interface WorkflowTaskResultOptions {
  name: string;
  status: WorkflowTaskStatusName;
  output?: string;
  sessionId?: string;
  stopReason?: TnyStopReason;
  blockedBy?: readonly string[];
  error?: Error;
}

export class WorkflowTaskResult {
  readonly name: string;
  readonly status: WorkflowTaskStatusName;
  readonly output: string;
  readonly sessionId: string;
  readonly stopReason?: TnyStopReason;
  readonly blockedBy: readonly string[];
  readonly error?: Error;
  readonly ok: boolean;
  constructor(options: WorkflowTaskResultOptions);
  toJSON(): {
    name: string;
    status: WorkflowTaskStatusName;
    outputBytes: number;
    sessionIdBytes: number;
    stopReason?: TnyStopReason;
    blockedBy: readonly string[];
    error?: string;
  };
}

export class WorkflowResult implements Iterable<readonly [string, WorkflowTaskResult]> {
  readonly results: readonly WorkflowTaskResult[];
  readonly size: number;
  readonly ok: boolean;
  readonly failed: readonly WorkflowTaskResult[];
  constructor(results: readonly WorkflowTaskResult[]);
  get(name: string): WorkflowTaskResult | undefined;
  require(name: string): WorkflowTaskResult;
  output(name: string): string;
  entries(): MapIterator<[string, WorkflowTaskResult]>;
  [Symbol.iterator](): MapIterator<[string, WorkflowTaskResult]>;
  raiseForFailure(): void;
  toJSON(): {
    ok: boolean;
    results: ReturnType<WorkflowTaskResult["toJSON"]>[];
  };
}

export interface WorkflowRunnerContext {
  readonly signal: AbortSignal;
}

export type WorkflowTaskRunner = (
  task: WorkflowTask,
  prompt: string,
  context: WorkflowRunnerContext,
) => WorkflowTaskExecution | WorkflowTaskExecutionOptions |
  Promise<WorkflowTaskExecution | WorkflowTaskExecutionOptions>;

export interface WorkflowOptions {
  /** Default native runtime. Required unless every task overrides it or runner is set. */
  runtime?: RuntimeOptions;
  /** Maximum number of simultaneously active task runners. Defaults to 4. */
  maxConcurrency?: number;
  /** Maximum combined UTF-8 bytes injected from direct dependencies. Defaults to 1 MiB. */
  maxDependencyBytes?: number;
  /** Replace native execution while retaining DAG scheduling and result semantics. */
  runner?: WorkflowTaskRunner;
  onEvent?: (task: WorkflowTask, event: TnyEvent) => void | Promise<void>;
  onPermission?: (
    task: WorkflowTask,
    event: PermissionRequestEvent,
  ) => PermissionDecisionName | 0 | 1 | 2 |
    Promise<PermissionDecisionName | 0 | 1 | 2>;
}

export interface WorkflowRunOptions {
  signal?: AbortSignal;
}

export class Workflow {
  readonly tasks: readonly WorkflowTask[];
  constructor(options?: WorkflowOptions);
  task(name: string, prompt: string, options?: WorkflowTaskOptions): this;
  add(name: string, prompt: string, options?: WorkflowTaskOptions): this;
  run(options?: WorkflowRunOptions): Promise<WorkflowResult>;
  toJSON(): {
    tasks: number;
    maxConcurrency: number;
    maxDependencyBytes: number;
    runner: "native" | "custom";
  };
}
