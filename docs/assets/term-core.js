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
  /* Named-provider env pairs (docs/adr/0018): OPENROUTER_API_KEY,
   * GROQ_BASE_URL, … ride the hash into the wasm module's environ, where
   * the CLI's own provider detection picks them up. */
  var ENV_PAIR_RE = /^[A-Z][A-Z0-9_]*_(API_KEY|BASE_URL|DEFAULT_MODEL|WIRE_API)$/;

  function assignSecret(out, key, value) {
    if (value == null) return;
    var trimmed = String(value).trim();
    if (!trimmed) return;
    var field = PARAM_MAP[key];
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
    parseParamString: parseParamString,
    takeSecretsFromLocation: takeSecretsFromLocation,
    sanitizeBaseUrl: sanitizeBaseUrl,
    sanitizeApiKey: sanitizeApiKey,
    wrapToCols: wrapToCols,
    proposeTermGeometry: proposeTermGeometry,
  };

  root.tnyTermCore = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
})(typeof globalThis !== "undefined" ? globalThis : this);
