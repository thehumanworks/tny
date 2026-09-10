/** Standalone toolkit configuration. Omitted credentials use the existing tny login. */
export interface ToolkitConfig {
  workspace?: string;
  settingsPath?: string;
  chatgptToken?: string;
  chatgptAccountId?: string;
  /** Trusted Codex media gateway; independent of the optimisation endpoint. */
  codexBaseUrl?: string;
  xaiApiKey?: string;
}

export interface ToolkitCallOptions { signal?: AbortSignal; provider?: string; }
export type ImageQuality = "auto" | "low" | "medium" | "high" | "xhigh" | "max";
export interface ImageOptions extends ToolkitCallOptions {
  outputFile: string;
  model?: string;
  quality?: ImageQuality;
  size?: string;
}
export interface ImageEditOptions extends ImageOptions { images: readonly string[]; }
export interface SpeechOptions extends ToolkitCallOptions {
  /** Omit to play speech locally; supply to export MP3 without playback. */
  outputFile?: string;
  voice?: string;
}
export interface DictationOptions extends ToolkitCallOptions {
  /** Required capture duration, 1–300 seconds. */
  seconds: number;
  device?: string;
}
export interface OptimisationOptions extends ToolkitCallOptions {
  model?: string;
  baseUrl?: string;
  apiKey?: string;
  wireApi?: "chat" | "responses";
  /** Positive integer, 1–86400; omission uses the native configuration/default. */
  timeoutSeconds?: number;
}
export interface ImageResult {
  readonly path: string;
  readonly mimeType: string;
  readonly byteCount: number;
  readonly provider: string;
  readonly model: string;
}
export interface SpeechResult {
  readonly path: string | null;
  readonly played: boolean;
  readonly provider: string;
  readonly voice: string;
  readonly mimeType: string;
}
export interface TranscriptionResult { readonly text: string; readonly provider: string; }
export interface OptimisationResult extends TranscriptionResult { readonly model: string; }

/** libtny ABI 1.2+. Each call owns a cancellable native job; no Runtime,
 * Session, CLI executable, or close/disposal is required. Paths resolve
 * against the captured workspace. Abort waits for native cleanup.
 */
export class Toolkit {
  constructor(config?: ToolkitConfig);
  generateImage(prompt: string, options: ImageOptions): Promise<ImageResult>;
  editImage(prompt: string, options: ImageEditOptions): Promise<ImageResult>;
  speak(text: string, options?: SpeechOptions): Promise<SpeechResult>;
  transcribe(inputFile: string, options?: ToolkitCallOptions): Promise<TranscriptionResult>;
  dictate(options: DictationOptions): Promise<TranscriptionResult>;
  optimise(text: string, options?: OptimisationOptions): Promise<OptimisationResult>;
  optimize(text: string, options?: OptimisationOptions): Promise<OptimisationResult>;
  toJSON(): { workspace: string };
}
