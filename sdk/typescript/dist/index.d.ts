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
  readonly abiMajor: 0;
  readonly abiMinor: number;
  readonly experimental: true;
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
