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
export type ImageSizeStatus = "auto" | "match" | "mismatch" | "unverifiable" | "unsupported";
export interface ImageOptions extends ToolkitCallOptions {
  outputFile: string;
  model?: string;
  quality?: ImageQuality;
  size?: string;
  /** Fail instead of saving unless the image is exactly the requested
   * WIDTHxHEIGHT. Requires an exact size; auto and provider size names are
   * rejected before any request. */
  strictSize?: boolean;
  /** Write the private per-operation manifest beside the output; default true.
   * False records no manifest, prompt or reference history at all. */
  persistManifest?: boolean;
  /** Rerun an earlier manifest: its prompt, references and settings are reused
   * unless given here, and the prompt argument may then be omitted. It must
   * record the same operation, and the output must be a new path. */
  fromManifest?: string;
}
export interface ImageEditOptions extends ImageOptions {
  /** Required unless artifact or fromManifest supplies the references. */
  images?: readonly string[];
  /** An earlier manifest whose verified output becomes the first reference. */
  artifact?: string;
}
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
  /** The literal size requested, and the literal size actually sent. */
  readonly requestedSize: string | null;
  readonly effectiveSize: string | null;
  /** Read from the returned bytes; null when the header could not be read. */
  readonly width: number | null;
  readonly height: number | null;
  readonly sizeStatus: ImageSizeStatus | null;
  /** This operation's identity, and the record written for it. `manifestPath`
   * is null when persistence was declined. */
  readonly operationId: string | null;
  readonly manifestPath: string | null;
  /** Only what the provider actually returned; null when it returned nothing. */
  readonly seed: number | null;
  readonly requestId: string | null;
}
/** Why a `strictSize` request was rejected, and what was not written.
 *
 * A failure type, never an `ImageResult`: no file exists, so `path` is always
 * `null` and `committed` always `false`. Every value is decided locally — the
 * requested size, the literal actually sent (`null` when nothing was), and
 * dimensions/MIME read from returned bytes. It contains no credential,
 * endpoint, prompt, reference path or provider response text. It is attached
 * to the rejected `TnyError` as a non-enumerable read-only `imageDetail`, so
 * `message`, `stack`, `JSON.stringify` and default inspection never show it.
 */
export interface ImageFailureDetail {
  readonly code: "IMAGE_STRICT_SIZE_INVALID" | "IMAGE_SIZE_MISMATCH"
    | "IMAGE_SIZE_UNVERIFIABLE" | "IMAGE_SIZE_UNSUPPORTED";
  readonly operation: "generate" | "edit";
  readonly message: string;
  readonly requestedSize: string;
  readonly effectiveSize: string | null;
  readonly width: number | null;
  readonly height: number | null;
  readonly sizeStatus: ImageSizeStatus;
  readonly mimeType: string | null;
  readonly path: null;
  readonly committed: false;
}
/** The generated image was written and kept; only its record failed.
 *
 * The one failure that names a file: `committed` is always `true` and `path`
 * is the artifact tny deliberately did not delete. Like `ImageFailureDetail`
 * it carries only locally decided metadata — no credential, endpoint, prompt,
 * reference path or provider response text — and rides the same non-enumerable
 * read-only `imageDetail`. It is never an `ImageResult`, and the two failure
 * shapes are discriminated on `committed`.
 */
export interface RetainedImageDetail {
  readonly code: "IMAGE_MANIFEST_FINALIZE_FAILED";
  readonly operation: "generate" | "edit";
  readonly message: string;
  readonly path: string;
  readonly byteCount: number;
  readonly requestedSize: string;
  readonly effectiveSize: string | null;
  readonly width: number | null;
  readonly height: number | null;
  readonly sizeStatus: ImageSizeStatus;
  readonly mimeType: string | null;
  readonly operationId: string | null;
  readonly manifestPath: string | null;
  readonly committed: true;
}
/** What `imageDetail` may hold, discriminated on `committed`. */
export type ImageDetail = ImageFailureDetail | RetainedImageDetail;
/** The failed `TnyError` as seen by a caller that deliberately inspects the
 * detail; `imageDetail` is absent for every other failure. */
export type ImageFailure = Error & {
  readonly status: number;
  readonly imageDetail?: ImageDetail;
};
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
  generateImage(prompt: string | null | undefined, options: ImageOptions): Promise<ImageResult>;
  editImage(prompt: string | null | undefined, options: ImageEditOptions): Promise<ImageResult>;
  speak(text: string, options?: SpeechOptions): Promise<SpeechResult>;
  transcribe(inputFile: string, options?: ToolkitCallOptions): Promise<TranscriptionResult>;
  dictate(options: DictationOptions): Promise<TranscriptionResult>;
  optimise(text: string, options?: OptimisationOptions): Promise<OptimisationResult>;
  optimize(text: string, options?: OptimisationOptions): Promise<OptimisationResult>;
  toJSON(): { workspace: string };
}
