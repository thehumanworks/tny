import { resolve } from "node:path";
import { Buffer } from "node:buffer";

const configFields = {
  workspace: "workspace", settingsPath: "settings_path", chatgptToken: "chatgpt_token",
  chatgptAccountId: "chatgpt_account_id", codexBaseUrl: "codex_base_url", xaiApiKey: "xai_api_key",
};
const imageFields = {
  outputFile: "output_file", provider: "provider", model: "model", quality: "quality",
  size: "size", strictSize: "strict_size", persistManifest: "persist_manifest",
  fromManifest: "from_manifest",
};
const optimiseFields = {
  provider: "provider", model: "model", baseUrl: "base_url", apiKey: "api_key",
  wireApi: "wire_api", timeoutSeconds: "timeout_seconds",
};

function text(value, name) {
  if (typeof value !== "string" || !value || value.includes("\0") ||
      /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(^|[^\uD800-\uDBFF])[\uDC00-\uDFFF]/u.test(value)) {
    throw new TypeError(`${name} must be nonempty UTF-8 text without NUL`);
  }
  return value;
}

function snapshot(options, mapping, withSignal = true) {
  if (!options || typeof options !== "object" || Array.isArray(options)) {
    throw new TypeError("toolkit options must be an object");
  }
  const result = Object.create(null);
  let signal;
  for (const key of Object.keys(options)) {
    // Accessor-backed values are read exactly once, and only primitives enter
    // the JSON envelope. Never pass arbitrary objects/toJSON to native work.
    const value = options[key];
    if (key === "signal" && withSignal) {
      if (value !== undefined && !(value instanceof AbortSignal)) {
        throw new TypeError("signal must be an AbortSignal");
      }
      signal = value;
    } else if (!Object.hasOwn(mapping, key)) {
      throw new TypeError("unknown toolkit option");
    } else if (value !== undefined) {
      if (key === "images") {
        if (!Array.isArray(value) || value.length < 1 || value.length > 5) {
          throw new TypeError("images must contain one to five file paths");
        }
        result.images = Array.from(value, item => text(item, "image path"));
      } else if (key === "strictSize" || key === "persistManifest") {
        if (typeof value !== "boolean") throw new TypeError(`${key} must be a boolean`);
        result[mapping[key]] = value;
      } else if (key === "seconds" || key === "timeoutSeconds") {
        const max = key === "seconds" ? 300 : 86400;
        if (!Number.isInteger(value) || value < 1 || value > max) {
          throw new TypeError(`${key} must be an integer from 1 to ${max}`);
        }
        result[mapping[key]] = value;
      } else result[mapping[key]] = text(value, key);
    }
  }
  return { values: result, signal };
}

function imageResult(value) {
  // Dimensions come from the returned bytes, and provenance from the record
  // actually written; unknown and declined values stay null.
  return Object.freeze({
    path: value.path, mimeType: value.mime_type, byteCount: value.bytes,
    provider: value.provider, model: value.model,
    requestedSize: value.requested_size ?? null, effectiveSize: value.effective_size ?? null,
    width: value.width ?? null, height: value.height ?? null,
    sizeStatus: value.size_status ?? null,
    operationId: value.operation_id ?? null, manifestPath: value.manifest_path ?? null,
    seed: value.seed ?? null, requestId: value.request_id ?? null,
  });
}

// The exact codes libtny assigns locally for strictSize (docs/images.md).
const strictImageCodes = new Set([
  "IMAGE_STRICT_SIZE_INVALID", "IMAGE_SIZE_MISMATCH",
  "IMAGE_SIZE_UNVERIFIABLE", "IMAGE_SIZE_UNSUPPORTED",
]);
// The one failure that keeps a written file (docs/images.md, ADR 0095).
const retainedImageCode = "IMAGE_MANIFEST_FINALIZE_FAILED";
const sizeStatuses = new Set(["auto", "match", "mismatch", "unverifiable", "unsupported"]);
const detailFields = [
  "kind", "ok", "operation", "code", "error", "mime_type", "requested_size",
  "effective_size", "width", "height", "size_status", "path", "committed",
];
const retainedFields = [...detailFields, "bytes", "operation_id", "manifest_path"];

function detailText(value, optional = false) {
  return value === null ? optional : typeof value === "string" && value.length > 0;
}

function detailDimension(value) {
  return value === null || (Number.isInteger(value) && value > 0);
}

/** The metadata both failure shapes carry, all of it locally decided. */
function sharedDetailFields(value) {
  return value.kind === "image" && value.ok === false &&
    (value.operation === "generate" || value.operation === "edit") &&
    sizeStatuses.has(value.size_status) && detailText(value.error) &&
    detailText(value.requested_size) && detailText(value.effective_size, true) &&
    detailText(value.mime_type, true) && detailDimension(value.width) &&
    detailDimension(value.height);
}

/** Accept only a complete retained-artifact object: the image was written and
 * kept, and only its manifest failed. Never parsed by the strict reader. */
function retainedDetail(value) {
  if (!retainedFields.every(field => Object.hasOwn(value, field))) return null;
  if (value.code !== retainedImageCode || value.committed !== true ||
      !detailText(value.path) || !Number.isInteger(value.bytes) || value.bytes < 0 ||
      !detailText(value.operation_id, true) || !detailText(value.manifest_path, true) ||
      !sharedDetailFields(value)) {
    return null;
  }
  return Object.freeze({
    code: value.code, operation: value.operation, message: value.error,
    path: value.path, byteCount: value.bytes,
    requestedSize: value.requested_size, effectiveSize: value.effective_size,
    width: value.width, height: value.height, sizeStatus: value.size_status,
    mimeType: value.mime_type, operationId: value.operation_id,
    manifestPath: value.manifest_path, committed: true,
  });
}

/** Accept only a complete, locally shaped image failure object, discriminated
 * on `committed`. This is a failure type: it is never parsed as, or coerced
 * into, an ImageResult, and the two shapes never share a reader. */
function imageDetail(json) {
  let value;
  try { value = JSON.parse(json); } catch { return null; }
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  if (!detailFields.every(field => Object.hasOwn(value, field))) return null;
  if (value.code === retainedImageCode || value.committed !== false) return retainedDetail(value);
  if (value.path !== null || !strictImageCodes.has(value.code) || !sharedDetailFields(value)) {
    return null;
  }
  return Object.freeze({
    code: value.code, operation: value.operation, message: value.error,
    requestedSize: value.requested_size, effectiveSize: value.effective_size,
    width: value.width, height: value.height, sizeStatus: value.size_status,
    mimeType: value.mime_type, path: null, committed: false,
  });
}

/** Attach the validated detail without touching message/stack/enumeration. */
function withImageDetail(error) {
  const raw = error?.imageDetailJson;
  if (typeof raw !== "string" || !Object.isExtensible(error)) return error;
  const detail = imageDetail(raw);
  if (detail) {
    Object.defineProperty(error, "imageDetail", {
      value: detail, enumerable: false, writable: false, configurable: false,
    });
  }
  return error;
}

/** The package entry point supplies its verified native binding and errors. */
export function createToolkitClass(native, invoke, TnyError) {
  return class Toolkit {
    #config;

    constructor(config = {}) {
      const { values } = snapshot(config, configFields, false);
      values.workspace = resolve(values.workspace ?? process.cwd());
      this.#config = Object.freeze(values);
    }

    // Credentials remain private even under JSON serialization and inspection.
    toJSON() { return { workspace: this.#config.workspace }; }

    async #run(operation, request, signal) {
      if (signal?.aborted) throw new TnyError("toolkit operation cancelled", -12);
      const json = JSON.stringify({ version: 1, operation, config: this.#config, request });
      if (Buffer.byteLength(json, "utf8") > 256 * 1024) {
        throw new TypeError("toolkit request exceeds 256 KiB");
      }
      let job;
      try {
        job = native.startToolkit(json);
        signal?.addEventListener("abort", job.cancel, { once: true });
        if (signal?.aborted) job.cancel();
        return JSON.parse(await invoke(job.promise));
      } catch (error) {
        if (error?.name === "TnyError") Object.setPrototypeOf(error, TnyError.prototype);
        throw withImageDetail(error);
      } finally {
        if (job) signal?.removeEventListener("abort", job.cancel);
      }
    }

    // A rerun supplies the recorded prompt, so only then may it be omitted.
    #prompt(values, prompt) {
      if (prompt !== undefined && prompt !== null) values.prompt = text(prompt, "prompt");
      else if (!values.from_manifest) text(prompt, "prompt");
    }

    async generateImage(prompt, options) {
      const { values, signal } = snapshot(options, imageFields);
      this.#prompt(values, prompt);
      return imageResult(await this.#run("generate_image", values, signal));
    }

    async editImage(prompt, options) {
      const { values, signal } = snapshot(options,
        { ...imageFields, images: "images", artifact: "artifact" });
      this.#prompt(values, prompt);
      return imageResult(await this.#run("edit_image", values, signal));
    }

    async speak(input, options = {}) {
      const { values, signal } = snapshot(options, { outputFile: "output_file", provider: "provider", voice: "voice" });
      values.text = text(input, "text");
      const result = await this.#run("speak", values, signal);
      return Object.freeze({
        path: result.path, played: result.played, provider: result.provider,
        voice: result.voice, mimeType: result.mime_type,
      });
    }

    async transcribe(inputFile, options = {}) {
      const { values, signal } = snapshot(options, { provider: "provider" });
      values.input_file = text(inputFile, "inputFile");
      return Object.freeze(await this.#run("transcribe", values, signal));
    }

    async dictate(options) {
      const { values, signal } = snapshot(options, { seconds: "seconds", provider: "provider", device: "device" });
      return Object.freeze(await this.#run("dictate", values, signal));
    }

    async optimise(input, options = {}) {
      const { values, signal } = snapshot(options, optimiseFields);
      values.text = text(input, "text");
      return Object.freeze(await this.#run("optimise", values, signal));
    }

    optimize(input, options = {}) { return this.optimise(input, options); }
  };
}
