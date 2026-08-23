/* term-core.js — client-side helpers for the GitHub Pages terminal.
 * No DOM. Safe to load from <head> so secrets can leave the address bar
 * before the first paint. */
(function (root) {
  var DEFAULT_BASE = "https://api.openai.com/v1";
  var PARAM_MAP = {
    OPENAI_API_KEY: "apiKey",
    OPENAI_BASE_URL: "baseUrl",
    OPENAI_MODEL: "model",
    key: "apiKey",
    base: "baseUrl",
    model: "model",
  };
  var ENV_MAP = {
    OPENAI_API_KEY: "apiKey",
    OPENAI_BASE_URL: "baseUrl",
    OPENAI_MODEL: "model",
  };
  /* Named-provider env pairs (docs/adr/0018): OPENROUTER_API_KEY,
   * GROQ_BASE_URL, … ride the hash into the wasm module's environ, where
   * the CLI's own provider detection picks them up. */
  var ENV_PAIR_RE = /^[A-Z][A-Z0-9_]*_(API_KEY|BASE_URL|DEFAULT_MODEL|WIRE_API)$/;

  function assignSecret(out, key, value) {
    if (value == null) return;
    var trimmed = String(value).trim();
    if (!trimmed) return;
    var field = PARAM_MAP[key] || ENV_MAP[key];
    if (field) {
      out[field] = trimmed;
      return;
    }
    if (ENV_PAIR_RE.test(key)) {
      out.env = out.env || {};
      out.env[key] = trimmed;
    }
  }

  function parseParamString(raw) {
    var out = {};
    if (!raw) return out;
    var s = String(raw);
    if (s.charAt(0) === "#" || s.charAt(0) === "?") s = s.slice(1);
    if (s.charAt(0) === "?") s = s.slice(1);
    if (!s) return out;
    var params;
    try {
      params = new URLSearchParams(s);
    } catch (e) {
      return out;
    }
    params.forEach(function (value, key) {
      assignSecret(out, key, value);
    });
    return out;
  }

  function mergeSecrets(a, b) {
    var out = {};
    [a, b].forEach(function (src) {
      if (!src) return;
      if (src.apiKey) out.apiKey = src.apiKey;
      if (src.baseUrl) out.baseUrl = src.baseUrl;
      if (src.model) out.model = src.model;
      if (src.env) {
        out.env = out.env || {};
        for (var k in src.env) out.env[k] = src.env[k];
      }
    });
    return out;
  }

  function stripSecretParams(raw, leading) {
    if (!raw) return "";
    var s = String(raw);
    var prefix = "";
    if (s.charAt(0) === "#" || s.charAt(0) === "?") {
      prefix = s.charAt(0);
      s = s.slice(1);
    }
    if (s.charAt(0) === "?") s = s.slice(1);
    if (!s) return "";
    var params;
    try {
      params = new URLSearchParams(s);
    } catch (e) {
      return leading ? prefix : "";
    }
    Object.keys(PARAM_MAP).forEach(function (key) {
      params.delete(key);
    });
    var generic = [];
    params.forEach(function (_v, key) {
      if (ENV_PAIR_RE.test(key)) generic.push(key);
    });
    generic.forEach(function (key) {
      params.delete(key);
    });
    var kept = params.toString();
    if (!kept) return "";
    return (leading || prefix || "") + kept;
  }

  function takeSecretsFromLocation(loc, hist) {
    var fromSearch = parseParamString(loc && loc.search);
    var fromHash = parseParamString(loc && loc.hash);
    var got = mergeSecrets(fromSearch, fromHash);
    var dirty = !!(got.apiKey || got.baseUrl || got.model || got.env);
    if (dirty && hist && typeof hist.replaceState === "function") {
      var search = stripSecretParams(loc.search, "?");
      var hash = stripSecretParams(loc.hash, "#");
      var next = (loc.pathname || "/") + search + hash;
      try {
        hist.replaceState(null, "", next);
      } catch (e) {}
    }
    return got;
  }

  function unquote(value) {
    if (value.length >= 2) {
      var a = value.charAt(0);
      var b = value.charAt(value.length - 1);
      if ((a === '"' && b === '"') || (a === "'" && b === "'"))
        return value.slice(1, -1);
    }
    return value;
  }

  function parseEnvAssignments(line) {
    var out = {};
    if (!line) return out;
    var s = String(line).replace(/^\s*export\s+/, "");
    var re = /\b(OPENAI_API_KEY|OPENAI_BASE_URL|OPENAI_MODEL)\s*=\s*("(?:\\.|[^"])*"|'(?:\\.|[^'])*'|[^\s]+)/g;
    var m;
    while ((m = re.exec(s))) assignSecret(out, m[1], unquote(m[2]));
    return out;
  }

  function looksLikeSecretAssignment(line) {
    var got = parseEnvAssignments(line);
    return !!(got.apiKey || got.baseUrl);
  }

  function looksLikeSecretDraft(line) {
    if (!line) return false;
    if (/^\s*(export\s+)?(OPENAI_API_KEY|OPENAI_BASE_URL)\s*=/.test(line))
      return true;
    if (/^\/(login|setup)\b/i.test(line)) return true;
    return looksLikeSecretAssignment(line);
  }

  function sanitizeBaseUrl(url) {
    var raw = (url == null || url === "") ? DEFAULT_BASE : String(url).trim();
    if (!raw) raw = DEFAULT_BASE;
    var parsed;
    try {
      parsed = new URL(raw);
    } catch (e) {
      throw new Error("base url must be an absolute http(s) URL");
    }
    if (parsed.protocol !== "http:" && parsed.protocol !== "https:")
      throw new Error("base url must be http(s)");
    return raw.replace(/\/+$/, "");
  }

  /* fetch() requires header values to be ISO-8859-1; one code point > U+00FF
   * in "Bearer <key>" throws before any network I/O. Keys are pasted from
   * rich text (docs, chat apps), which injects NBSP, zero-width, bidi, and
   * smart-quote characters. Strip the invisible junk, then reject anything
   * left outside printable ASCII instead of sending a corrupted key. */
  var KEY_JUNK_RE = /[\s\u00a0\u00ad\u034f\u061c\u200b-\u200f\u202a-\u202e\u2060-\u206f\ufeff]/g;

  function sanitizeApiKey(value) {
    if (value == null) return "";
    var s = String(value).replace(KEY_JUNK_RE, "");
    for (var i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i);
      if (c < 0x21 || c > 0x7e)
        throw new Error(
          "API key contains a non-ASCII character (" +
            "U+" + ("0000" + c.toString(16)).slice(-4).toUpperCase() +
            " at position " + i +
            ") that cannot be sent in an HTTP header. Re-copy the key as plain text."
        );
    }
    return s;
  }

  function joinApi(base, path) {
    var b = sanitizeBaseUrl(base || DEFAULT_BASE);
    var p = path.charAt(0) === "/" ? path : "/" + path;
    return b + p;
  }

  function maskSecret(value) {
    if (value == null || value === "") return "";
    var s = String(value);
    if (s.length <= 8) return Array(s.length + 1).join("•");
    var keep = 3;
    var mid = Math.min(24, s.length - keep * 2);
    return s.slice(0, keep) + Array(mid + 1).join("•") + s.slice(-keep);
  }

  function obfuscateUrl(url) {
    if (url == null || url === "") return "••••";
    var parsed;
    try {
      parsed = new URL(String(url));
    } catch (e) {
      return "••••";
    }
    var host = parsed.hostname || "";
    var maskedHost;
    if (host.length <= 3) maskedHost = Array(host.length + 1).join("•");
    else {
      var inner = Math.min(10, host.length - 2);
      maskedHost =
        host.charAt(0) + Array(inner + 1).join("•") + host.charAt(host.length - 1);
    }
    var path = parsed.pathname && parsed.pathname !== "/" ? "/***" : "";
    return parsed.protocol + "//" + maskedHost + path;
  }

  function redactText(text, creds) {
    if (text == null || text === "") return text;
    var out = String(text);
    if (!creds) return out;
    if (creds.apiKey) out = out.split(creds.apiKey).join(maskSecret(creds.apiKey));
    if (creds.baseUrl) out = out.split(creds.baseUrl).join(obfuscateUrl(creds.baseUrl));
    return out;
  }

  function SseParser() {
    this.buf = "";
  }

  SseParser.prototype.push = function (chunk) {
    this.buf += chunk == null ? "" : String(chunk);
    var events = [];
    var idx;
    while ((idx = this.buf.indexOf("\n")) !== -1) {
      var line = this.buf.slice(0, idx);
      this.buf = this.buf.slice(idx + 1);
      if (line.charAt(line.length - 1) === "\r") line = line.slice(0, -1);
      if (line.indexOf("data:") !== 0) continue;
      var data = line.slice(5);
      if (data.charAt(0) === " ") data = data.slice(1);
      if (data === "[DONE]") events.push({ done: true });
      else {
        try {
          events.push({ json: JSON.parse(data) });
        } catch (e) {
          events.push({ raw: data });
        }
      }
    }
    return events;
  };

  /* ---- Responses API wire (docs/adr/0014) ----
   * The browser demo mirrors the CLI's native backend: history stays
   * chat-shaped in memory and translates onto `input` items per request. */

  function toResponsesInput(msgs) {
    var items = [];
    (msgs || []).forEach(function (m) {
      if (!m || !m.role) return;
      if (m.role === "tool") {
        items.push({
          type: "function_call_output",
          call_id: m.tool_call_id || "call_0",
          output: m.content == null ? "" : String(m.content),
        });
        return;
      }
      if (m.role === "assistant") {
        if (m.content) items.push({ role: "assistant", content: m.content });
        (m.tool_calls || []).forEach(function (tc) {
          var fn = (tc && tc.function) || {};
          items.push({
            type: "function_call",
            call_id: (tc && tc.id) || "call_0",
            name: fn.name || "unknown",
            arguments: fn.arguments || "{}",
          });
        });
        return;
      }
      items.push({ role: m.role, content: m.content == null ? "" : m.content });
    });
    return items;
  }

  /* Accumulates one streamed response from typed Responses events.
   * push(ev) returns the text delta to paint (usually ""). Tool calls come
   * out chat-shaped so the caller's history format never changes. */
  function ResponsesTurn() {
    this.content = "";
    this.calls = [];
    this.byIndex = {};
    this.completed = false;
    this.error = null;
  }

  ResponsesTurn.prototype.push = function (ev) {
    if (!ev || !ev.type) return "";
    var t = ev.type;
    if (t === "response.output_text.delta") {
      var d = ev.delta == null ? "" : String(ev.delta);
      this.content += d;
      return d;
    }
    if (t === "response.output_item.added" || t === "response.output_item.done") {
      var item = ev.item;
      if (!item || item.type !== "function_call") return "";
      var i = ev.output_index == null ? "n" + this.calls.length : ev.output_index;
      var call = this.byIndex[i];
      if (!call) {
        call = { id: "", type: "function", function: { name: "", arguments: "" } };
        this.byIndex[i] = call;
        this.calls.push(call);
      }
      if (item.call_id && !call.id) call.id = item.call_id;
      if (item.name && !call.function.name) call.function.name = item.name;
      /* done carries the complete argument string: authoritative over
       * deltas — but an empty string never wipes assembled deltas */
      if (item.arguments) call.function.arguments = item.arguments;
      return "";
    }
    if (t === "response.function_call_arguments.delta") {
      var c = this.byIndex[ev.output_index];
      if (c && ev.delta) c.function.arguments += ev.delta;
      return "";
    }
    if (t === "response.completed" || t === "response.incomplete") {
      this.completed = true;
      return "";
    }
    if (t === "response.failed" || t === "error") {
      var e = (ev.response && ev.response.error) || ev.error || {};
      this.error = e.message || ev.message || "response failed";
      this.completed = true;
      return "";
    }
    return "";
  };

  function bytesToB64(bytes) {
    var u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    var bin = "";
    for (var i = 0; i < u8.length; i++) bin += String.fromCharCode(u8[i]);
    return btoa(bin);
  }

  function b64ToBytes(b64) {
    var bin = atob(b64);
    var u8 = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
    return u8;
  }

  function aesGcmGenerateKey() {
    return crypto.subtle.generateKey(
      { name: "AES-GCM", length: 256 },
      false,
      ["encrypt", "decrypt"]
    );
  }

  function aesGcmSeal(key, plaintextU8) {
    var iv = crypto.getRandomValues(new Uint8Array(12));
    return crypto.subtle
      .encrypt({ name: "AES-GCM", iv: iv }, key, plaintextU8)
      .then(function (ct) {
        return { iv: iv, ct: new Uint8Array(ct) };
      });
  }

  function aesGcmOpen(key, iv, ct) {
    return crypto.subtle
      .decrypt({ name: "AES-GCM", iv: iv }, key, ct)
      .then(function (pt) {
        return new Uint8Array(pt);
      });
  }

  var WEB_CMDS = [
    ["help", "keys and commands"],
    ["clear", "clear the screen"],
    ["new", "start a new session"],
    ["reset", "new session and clear the screen"],
    ["login", "set OPENAI_API_KEY (not echoed)"],
    ["logout", "drop the encrypted key"],
    ["setup", "/setup OPENAI_API_KEY=… OPENAI_BASE_URL=…"],
    ["model", "/model [ID]"],
    ["models", "list provider models"],
    ["status", "provider, auth (redacted)"],
    ["permissions", "browser demo is yolo"],
    ["quit", "this is a web page"],
  ];

  /* Word-wrap for the pre-launch banner so a 30-col phone does not
   * mid-word-split the same way a hard-coded 70-col line would. Existing
   * newlines stay paragraph breaks. */
  function wrapToCols(text, cols) {
    var width = Math.max(1, cols | 0);
    var src = text == null ? "" : String(text);
    var paragraphs = src.split(/\r\n|\n|\r/);
    var out = [];
    for (var p = 0; p < paragraphs.length; p++) {
      var line = paragraphs[p];
      if (!line) {
        out.push("");
        continue;
      }
      var words = line.split(/[ \t]+/);
      var cur = "";
      for (var i = 0; i < words.length; i++) {
        var word = words[i];
        if (!word) continue;
        while (word.length > width) {
          if (cur) {
            out.push(cur);
            cur = "";
          }
          out.push(word.slice(0, width));
          word = word.slice(width);
        }
        if (!cur) cur = word;
        else if (cur.length + 1 + word.length <= width) cur += " " + word;
        else {
          out.push(cur);
          cur = word;
        }
      }
      if (cur) out.push(cur);
    }
    return out;
  }

  /* xterm.js defaults to 80×24. On a phone that is wider than the
   * container, so overflow:hidden clips the row. Floor to the cells that
   * actually fit; never invent extra columns. */
  function proposeTermGeometry(widthPx, heightPx, cellW, cellH) {
    var w = Math.max(0, Number(widthPx) || 0);
    var h = Math.max(0, Number(heightPx) || 0);
    var cw = Math.max(1, Number(cellW) || 1);
    var ch = Math.max(1, Number(cellH) || 1);
    return {
      cols: Math.max(2, Math.floor(w / cw)),
      rows: Math.max(1, Math.floor(h / ch)),
    };
  }

  var api = {
    DEFAULT_BASE: DEFAULT_BASE,
    PARAM_MAP: PARAM_MAP,
    parseParamString: parseParamString,
    mergeSecrets: mergeSecrets,
    stripSecretParams: stripSecretParams,
    takeSecretsFromLocation: takeSecretsFromLocation,
    parseEnvAssignments: parseEnvAssignments,
    looksLikeSecretAssignment: looksLikeSecretAssignment,
    looksLikeSecretDraft: looksLikeSecretDraft,
    sanitizeBaseUrl: sanitizeBaseUrl,
    sanitizeApiKey: sanitizeApiKey,
    joinApi: joinApi,
    maskSecret: maskSecret,
    obfuscateUrl: obfuscateUrl,
    redactText: redactText,
    wrapToCols: wrapToCols,
    proposeTermGeometry: proposeTermGeometry,
    SseParser: SseParser,
    toResponsesInput: toResponsesInput,
    ResponsesTurn: ResponsesTurn,
    bytesToB64: bytesToB64,
    b64ToBytes: b64ToBytes,
    aesGcmGenerateKey: aesGcmGenerateKey,
    aesGcmSeal: aesGcmSeal,
    aesGcmOpen: aesGcmOpen,
    WEB_CMDS: WEB_CMDS,
  };

  root.tnyTermCore = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
})(typeof globalThis !== "undefined" ? globalThis : this);
