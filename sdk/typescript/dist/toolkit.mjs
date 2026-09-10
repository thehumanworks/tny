import { resolve } from "node:path";
import { Buffer } from "node:buffer";

const configFields = {
  workspace: "workspace", settingsPath: "settings_path", chatgptToken: "chatgpt_token",
  chatgptAccountId: "chatgpt_account_id", codexBaseUrl: "codex_base_url", xaiApiKey: "xai_api_key",
};
const imageFields = { outputFile: "output_file", provider: "provider", model: "model", quality: "quality", size: "size" };
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
  return Object.freeze({
    path: value.path, mimeType: value.mime_type, byteCount: value.bytes,
    provider: value.provider, model: value.model,
  });
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
        throw error;
      } finally {
        if (job) signal?.removeEventListener("abort", job.cancel);
      }
    }

    async generateImage(prompt, options) {
      const { values, signal } = snapshot(options, imageFields);
      values.prompt = text(prompt, "prompt");
      return imageResult(await this.#run("generate_image", values, signal));
    }

    async editImage(prompt, options) {
      const { values, signal } = snapshot(options, { ...imageFields, images: "images" });
      values.prompt = text(prompt, "prompt");
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
