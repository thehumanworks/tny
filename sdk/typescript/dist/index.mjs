import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { resolveNativeAddon } from "../scripts/native-loader.mjs";

const require = createRequire(import.meta.url);
const packageRoot = dirname(dirname(fileURLToPath(import.meta.url)));
let native;
try {
  const { addonPath } = resolveNativeAddon({ packageRoot });
  native = require(addonPath);
} catch (cause) {
  if (cause?.name === "TnyLoadError") throw cause;
  const error = new Error(
    `@thehumanworks/tny: verified native addon failed to load for ` +
      `${process.platform}-${process.arch}: ${cause?.message || cause}`,
    { cause },
  );
  error.name = "TnyLoadError";
  throw error;
}

export const EVENT_SCHEMA_VERSION = 1;

export const eventKinds = Object.freeze({
  text_delta: 0,
  thinking: 1,
  tool_start: 2,
  tool_end: 3,
  permission_request: 4,
  plan: 5,
  usage: 6,
  turn_end: 7,
  error: 8,
  status: 9,
  steer_rejected: 10,
  custom_message: 11,
  user_message: 12,
  tool_progress: 13,
});

export const PermissionMode = Object.freeze({ ask: 0, auto: 1, yolo: 2 });
export const PermissionDecision = Object.freeze({ allow: 0, allowAlways: 1, deny: 2 });
export const PermissionOption = Object.freeze({ allow: 1, allowAlways: 2, deny: 4 });

export class TnyError extends Error {
  constructor(message, status, options) {
    super(message, options);
    this.name = "TnyError";
    this.status = status;
  }
}

export class UnsupportedFeatureError extends TnyError {
  constructor(feature) {
    super(
      `${feature} is not advertised by this libtny ABI 1 capability snapshot`,
      -9,
    );
    this.name = "UnsupportedFeatureError";
    this.feature = feature;
  }
}

function normalizeCapabilities(capabilities) {
  return Object.freeze({
    ...capabilities,
    abiMajor: capabilities.abiVersion >>> 16,
    abiMinor: capabilities.abiVersion & 0xffff,
    experimental: false,
  });
}

function defineOwn(object, name, value) {
  const descriptor = Object.create(null);
  descriptor.value = value;
  descriptor.enumerable = true;
  descriptor.configurable = false;
  descriptor.writable = false;
  Object.defineProperty(object, name, descriptor);
}

async function invoke(promise) {
  try {
    return await promise;
  } catch (error) {
    if (error && error.name === "TnyError") {
      Object.setPrototypeOf(error, TnyError.prototype);
    }
    throw error;
  }
}

function permissionMode(value) {
  if (value === undefined) return PermissionMode.ask;
  if (typeof value === "string" && value in PermissionMode) return PermissionMode[value];
  if (value === 0 || value === 1 || value === 2) return value;
  throw new TypeError("permissionMode must be 'ask', 'auto', 'yolo', 0, 1, or 2");
}

function permissionDecision(value) {
  if (typeof value === "string" && value in PermissionDecision) return PermissionDecision[value];
  if (value === 0 || value === 1 || value === 2) return value;
  throw new TypeError("permission decision must be 'allow', 'allowAlways', 'deny', 0, 1, or 2");
}

const runtimeFinalizer = new FinalizationRegistry((runtimeId) => {
  native.closeRuntime(runtimeId).catch(() => {});
});
const sessionFinalizer = new FinalizationRegistry(({ runtimeId, sessionHandle }) => {
  native.closeSession(runtimeId, sessionHandle).catch(() => {});
});

export class Runtime {
  #runtimeId;
  #closed = false;
  #closing;
  #sessions = new Map();
  #finalizerToken = {};

  constructor(nativeInfo) {
    this.#runtimeId = nativeInfo.runtimeId;
    defineOwn(this, "abiVersion", nativeInfo.abiVersion);
    defineOwn(this, "libraryVersion", nativeInfo.libraryVersion);
    defineOwn(this, "capabilities", normalizeCapabilities(nativeInfo.capabilities));
    runtimeFinalizer.register(this, this.#runtimeId, this.#finalizerToken);
  }

  static async create(options) {
    if (!options || typeof options !== "object") throw new TypeError("Runtime.create requires options");
    if (typeof options.workspace !== "string") {
      throw new TypeError("workspace is a required string");
    }
    if (options.persistence && typeof options.stateDir !== "string") {
      throw new TypeError("stateDir is required when persistence is true");
    }
    if (options.provider !== undefined && options.provider !== "openai") {
      throw new UnsupportedFeatureError(`provider ${String(options.provider)}`);
    }
    const nativeInfo = await invoke(
      native.createRuntime({
        ...options,
        provider: options.provider ?? "openai",
        permissionMode: permissionMode(options.permissionMode),
      }),
    );
    return new Runtime(nativeInfo);
  }

  get closed() {
    return this.#closed;
  }

  async createSession() {
    this.#assertOpen();
    const info = await invoke(native.createSession(this.#runtimeId));
    return this.#adoptSession(info);
  }

  async openSession(sessionId) {
    this.#assertOpen();
    if (typeof sessionId !== "string" || sessionId.length === 0) {
      throw new TypeError("sessionId must be a non-empty string");
    }
    const info = await invoke(native.openSession(this.#runtimeId, sessionId));
    return this.#adoptSession(info);
  }

  async getCapabilities() {
    this.#assertOpen();
    return normalizeCapabilities(await invoke(native.getCapabilities(this.#runtimeId)));
  }

  async close() {
    if (this.#closed) return;
    if (this.#closing) return this.#closing;
    const attempt = invoke(native.closeRuntime(this.#runtimeId)).then(() => {
      runtimeFinalizer.unregister(this.#finalizerToken);
      for (const reference of this.#sessions.values()) reference.deref()?._markRuntimeClosed();
      this.#sessions.clear();
      this.#closed = true;
    }).finally(() => {
      if (!this.#closed) this.#closing = undefined;
    });
    this.#closing = attempt;
    return attempt;
  }

  async [Symbol.asyncDispose]() {
    await this.close();
  }

  #adoptSession(info) {
    const session = new Session(this, this.#runtimeId, info.sessionHandle, info.sessionId);
    this.#sessions.set(info.sessionHandle, new WeakRef(session));
    return session;
  }

  _forget(sessionHandle) {
    this.#sessions.delete(sessionHandle);
  }

  #assertOpen() {
    if (this.#closed || this.#closing) throw new TnyError("runtime is closing or closed", -2);
  }
}

export class Session {
  #runtime;
  #runtimeId;
  #sessionHandle;
  #closed = false;
  #closing;
  #active = false;
  #finalizerToken = {};

  constructor(runtime, runtimeId, sessionHandle, sessionId) {
    this.#runtime = runtime;
    this.#runtimeId = runtimeId;
    this.#sessionHandle = sessionHandle;
    defineOwn(this, "id", sessionId);
    sessionFinalizer.register(
      this,
      { runtimeId, sessionHandle },
      this.#finalizerToken,
    );
  }

  get closed() {
    return this.#closed;
  }

  async *run(prompt, options = {}) {
    this.#assertOpen();
    if (this.#active) throw new TnyError("a turn is already active on this session", -3);
    if (typeof prompt !== "string" || prompt.length === 0) {
      throw new TypeError("prompt must be a non-empty string");
    }
    if (options.images?.length) throw new UnsupportedFeatureError("images");
    if (options.outputSchema) throw new UnsupportedFeatureError("outputSchema");
    this.#active = true;
    let drained = false;
    let abortRequested = false;
    let abortPromise;
    const requestAbort = () => {
      abortRequested = true;
      abortPromise ??= this.#abort().then(
        () => ({ ok: true }),
        (error) => ({ ok: false, error }),
      );
    };
    try {
      const sendPromise = invoke(native.send(this.#runtimeId, this.#sessionHandle, prompt));
      if (options.signal) {
        if (options.signal.aborted) requestAbort();
        else options.signal.addEventListener("abort", requestAbort, { once: true });
      }
      await sendPromise;
      if (abortRequested) {
        await abortPromise;
        abortPromise = this.#abort().then(
          () => ({ ok: true }),
          (error) => ({ ok: false, error }),
        );
        const activeAbort = await abortPromise;
        if (!activeAbort.ok) throw activeAbort.error;
      }
      for (;;) {
        const item = await invoke(native.nextEvent(this.#runtimeId, this.#sessionHandle));
        if (item.done) {
          drained = true;
          break;
        }
        yield item.value;
      }
    } finally {
      options.signal?.removeEventListener("abort", requestAbort);
      if (abortPromise) {
        const result = await abortPromise;
        if (!result.ok && !drained) throw result.error;
      }
      if (!drained && !this.#closed) {
        if (!abortRequested) {
          try { await this.#abort(); } catch {}
        }
        try {
          for (;;) {
            const item = await invoke(native.nextEvent(this.#runtimeId, this.#sessionHandle));
            if (item.done) break;
          }
        } catch {}
      }
      this.#active = false;
    }
  }

  async ask(prompt, options = {}) {
    let text = "";
    let stopReason;
    let usage;
    for await (const event of this.run(prompt, options)) {
      if (event.type === "text_delta") text += event.text;
      else if (event.type === "turn_end") stopReason = event.stopReason;
      else if (event.type === "usage") usage = event;
      if (options.onEvent) await options.onEvent(event, this);
    }
    return Object.freeze({ text, stopReason, usage });
  }

  async respondPermission(requestId, decision) {
    this.#assertOpen();
    if (typeof requestId !== "string" || requestId.length === 0) {
      throw new TypeError("requestId must be a non-empty string");
    }
    await invoke(
      native.respondPermission(
        this.#runtimeId,
        this.#sessionHandle,
        requestId,
        permissionDecision(decision),
      ),
    );
  }

  async steer(text) {
    this.#assertOpen();
    if (typeof text !== "string" || text.length === 0) {
      throw new TypeError("steer text must be a non-empty string");
    }
    await invoke(native.steer(this.#runtimeId, this.#sessionHandle, text));
  }

  async cancel() {
    this.#assertOpen();
    await invoke(native.cancel(this.#runtimeId, this.#sessionHandle));
  }

  async close() {
    if (this.#closed) return;
    if (this.#closing) return this.#closing;
    const attempt = invoke(native.closeSession(this.#runtimeId, this.#sessionHandle)).then(() => {
      sessionFinalizer.unregister(this.#finalizerToken);
      this.#runtime?._forget(this.#sessionHandle);
      this.#closed = true;
    }).finally(() => {
      if (!this.#closed) this.#closing = undefined;
    });
    this.#closing = attempt;
    return attempt;
  }

  async [Symbol.asyncDispose]() {
    await this.close();
  }

  _markRuntimeClosed() {
    this.#closed = true;
    this.#runtime = undefined;
    sessionFinalizer.unregister(this.#finalizerToken);
  }

  #assertOpen() {
    if (this.#closed || this.#closing) throw new TnyError("session is closing or closed", -2);
  }

  async #abort() {
    for (;;) {
      try {
        await invoke(native.abort(this.#runtimeId, this.#sessionHandle));
        return;
      } catch (error) {
        if (!(error instanceof TnyError) || error.status !== -11 || this.#closed) throw error;
        await new Promise((resolve) => setTimeout(resolve, 0));
      }
    }
  }
}
