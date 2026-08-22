/* term.js — live GitHub Pages terminal. BYOK, client-side only.
 * Credentials are AES-GCM sealed in IndexedDB. The raw base URL is never
 * written into the DOM. */
(function () {
  var C = window.tnyTermCore;
  if (!C) return;

  var VERSION = "0.1.0";
  var IDB_NAME = "tny.web.v1";
  var IDB_STORE = "vault";
  var DEFAULT_MODEL = "gpt-4.1-mini";
  var STEP_LIMIT = 8;
  var SYS =
    "You are the in-browser demo of tny, a tiny C11 coding-agent harness. " +
    "You run entirely in this tab. You cannot read the visitor's disk, spawn " +
    "host binaries, or talk to Cursor/Codex. Answer questions about tny and " +
    "help with code they paste. Prefer short, concrete answers. Use " +
    "lookup_docs when asked how tny works.";

  var DOCS = {
    install:
      "Clone https://github.com/thehumanworks/tny && cd tny && make. C11 compiler + make. Host agents stay on PATH.",
    providers:
      "openai (OPENAI_API_KEY / optional OPENAI_BASE_URL), cursor (CURSOR_API_KEY + cursor-sdk-bridge), codex (app-server WS), acp (client or `tny acp`).",
    cli: "tny (TUI), tny ask, tny resume, tny acp, tny doctor. Leading --provider / --model / --permission-mode (default yolo).",
    tui: "Unix shell, not an IDE. Transcript + composer + status. / commands, @ files, $ skills. Esc interrupts.",
    permissions:
      "Default yolo for every provider. ask/auto are opt-ins on the native openai loop only.",
    backends:
      "Host loops: cursor sdk.v1 Connect, codex app-server WS, ACP stdio. Native: OpenAI-compatible Responses API + tools (wire_api \"chat\" for legacy gateways).",
    size: "Stripped tny is 0.41 MiB on macOS arm64 vs fx 6.4 MiB. --version ~1.7 ms. TUI first prompt 3–4 ms.",
  };

  /* Responses API tool shape: flat, no nested "function" (docs/adr/0014). */
  var TOOLS = [
    {
      type: "function",
      name: "lookup_docs",
      description: "Look up a tny documentation topic.",
      parameters: {
        type: "object",
        properties: {
          topic: {
            type: "string",
            description: "install, providers, cli, tui, permissions, backends, size",
          },
        },
        required: ["topic"],
      },
    },
  ];

  var root = document.getElementById("tny-term");
  if (!root) return;
  var transcript = root.querySelector("[data-term-transcript]");
  var input = root.querySelector("[data-term-input]");
  var statusEl = root.querySelector("[data-term-status]");
  var overlayEl = root.querySelector("[data-term-overlay]");
  var barEl = root.querySelector("[data-term-bar]");
  if (!transcript || !input || !statusEl) return;

  var creds = { apiKey: "", baseUrl: "", model: DEFAULT_MODEL };
  var messages = [];
  var history = [];
  var histIdx = -1;
  var draft = "";
  var busy = false;
  var abortCtl = null;
  var persist = "memory";
  var wrapKey = null;
  var unlocked = false;
  var secretMode = null;
  var lastReply = "";

  function text(s) {
    return document.createTextNode(s == null ? "" : String(s));
  }

  function line(kind, body) {
    var el = document.createElement("div");
    el.className = "term-line term-line-" + kind;
    el.appendChild(text(C.redactText(body, creds)));
    transcript.appendChild(el);
    transcript.scrollTop = transcript.scrollHeight;
    return el;
  }

  function sys(msg) {
    line("sys", msg);
  }
  function err(msg) {
    line("err", msg);
  }

  function hideOverlay() {
    if (!overlayEl) return;
    overlayEl.hidden = true;
    overlayEl.replaceChildren();
  }

  function showOverlay(rows) {
    if (!overlayEl) return;
    overlayEl.replaceChildren();
    rows.forEach(function (row) {
      var el = document.createElement("div");
      el.className = "term-overlay-row";
      el.appendChild(text(row));
      overlayEl.appendChild(el);
    });
    overlayEl.hidden = false;
  }

  function paintStatus() {
    var ready = creds.apiKey ? "ready" : "no key";
    statusEl.replaceChildren();
    var wrap = document.createElement("span");
    wrap.className = "status-row";
    var yolo = document.createElement("span");
    yolo.className = "auto";
    yolo.textContent = "yolo";
    wrap.appendChild(yolo);
    wrap.appendChild(text(" · openai"));
    if (creds.model) wrap.appendChild(text(" · " + creds.model));
    wrap.appendChild(text(" · " + ready));
    statusEl.appendChild(wrap);
  }

  function paintBar() {
    if (!barEl) return;
    barEl.hidden = !!(input.value || secretMode);
  }

  function resizeInput() {
    input.style.height = "19px";
    input.style.height = Math.min(input.scrollHeight, 76) + "px";
    var hide = secretMode || C.looksLikeSecretDraft(input.value);
    input.classList.toggle("is-secret", hide);
    input.setAttribute("aria-label", hide ? "Secret (hidden)" : "Prompt");
    paintBar();
  }

  function setBusy(on) {
    busy = on;
    input.readOnly = on;
    root.classList.toggle("is-busy", on);
  }

  function idbOpen() {
    return new Promise(function (resolve, reject) {
      if (!window.indexedDB) {
        reject(new Error("no indexedDB"));
        return;
      }
      var req = indexedDB.open(IDB_NAME, 1);
      req.onupgradeneeded = function () {
        if (!req.result.objectStoreNames.contains(IDB_STORE))
          req.result.createObjectStore(IDB_STORE);
      };
      req.onsuccess = function () {
        resolve(req.result);
      };
      req.onerror = function () {
        reject(req.error || new Error("idb open"));
      };
    });
  }

  function idbOp(mode, fn) {
    return idbOpen().then(function (db) {
      return new Promise(function (resolve, reject) {
        var tx = db.transaction(IDB_STORE, mode);
        var store = tx.objectStore(IDB_STORE);
        var req = fn(store);
        req.onsuccess = function () {
          resolve(req.result);
        };
        req.onerror = function () {
          reject(req.error);
        };
      });
    });
  }

  function loadWrapKey() {
    return idbOp("readonly", function (s) {
      return s.get("wrap");
    }).then(function (existing) {
      if (existing) return existing;
      return C.aesGcmGenerateKey().then(function (key) {
        return idbOp("readwrite", function (s) {
          return s.put(key, "wrap");
        }).then(function () {
          return key;
        });
      });
    });
  }

  function persistCreds() {
    if (!window.crypto || !crypto.subtle) {
      persist = "memory";
      return Promise.resolve();
    }
    var ready = wrapKey
      ? Promise.resolve(wrapKey)
      : loadWrapKey().then(function (key) {
          wrapKey = key;
          return key;
        });
    var payload = JSON.stringify({
      k: creds.apiKey,
      u: creds.baseUrl,
      m: creds.model,
    });
    var pt = new TextEncoder().encode(payload);
    return ready
      .then(function (key) {
        return C.aesGcmSeal(key, pt);
      })
      .then(function (sealed) {
        var blob = {
          v: 1,
          iv: C.bytesToB64(sealed.iv),
          ct: C.bytesToB64(sealed.ct),
        };
        return idbOp("readwrite", function (s) {
          return s.put(blob, "blob");
        });
      })
      .then(function () {
        persist = "idb";
      })
      .catch(function () {
        persist = "memory";
      });
  }

  function applySecrets(got, silent) {
    if (!got) return false;
    var changed = false;
    if (got.apiKey) {
      creds.apiKey = got.apiKey;
      changed = true;
    }
    if (got.baseUrl) {
      try {
        creds.baseUrl = C.sanitizeBaseUrl(got.baseUrl);
        changed = true;
      } catch (e) {
        err(e.message || "bad OPENAI_BASE_URL");
        return false;
      }
    }
    if (got.model) {
      creds.model = got.model;
      changed = true;
    }
    if (!changed) return false;
    unlocked = true;
    paintStatus();
    persistCreds();
    if (!silent) {
      if (got.apiKey)
        sys(
          got.baseUrl
            ? "key accepted · custom base " + C.obfuscateUrl(creds.baseUrl)
            : "key accepted · encrypted in this tab"
        );
      else if (got.baseUrl) sys("base set · " + C.obfuscateUrl(creds.baseUrl));
      else if (got.model) sys("model set");
    }
    return true;
  }

  function restoreVault() {
    if (!window.crypto || !crypto.subtle) return Promise.resolve();
    return loadWrapKey()
      .then(function (key) {
        wrapKey = key;
        return idbOp("readonly", function (s) {
          return s.get("blob");
        });
      })
      .then(function (blob) {
        if (unlocked || !blob || blob.v !== 1 || !blob.iv || !blob.ct) return;
        return C.aesGcmOpen(wrapKey, C.b64ToBytes(blob.iv), C.b64ToBytes(blob.ct)).then(
          function (pt) {
            if (unlocked) return;
            var data = JSON.parse(new TextDecoder().decode(pt));
            creds.apiKey = data.k || "";
            creds.baseUrl = data.u || "";
            creds.model = data.m || DEFAULT_MODEL;
            persist = "idb";
            paintStatus();
          }
        );
      })
      .catch(function () {
        persist = "memory";
      });
  }

  function wipeVault() {
    creds.apiKey = "";
    creds.baseUrl = "";
    creds.model = DEFAULT_MODEL;
    persist = "memory";
    paintStatus();
    return idbOp("readwrite", function (s) {
      return s.delete("blob");
    }).catch(function () {});
  }

  function lookupDocs(topic) {
    var key = String(topic || "")
      .toLowerCase()
      .replace(/[^a-z]+/g, "");
    if (DOCS[key]) return DOCS[key];
    return "unknown topic. try: " + Object.keys(DOCS).join(", ");
  }

  function execTool(name, argsJson) {
    var args = {};
    try {
      args = argsJson ? JSON.parse(argsJson) : {};
    } catch (e) {
      return "bad arguments";
    }
    if (name === "lookup_docs") return lookupDocs(args.topic);
    return "unknown tool";
  }

  function headers() {
    return {
      Authorization: "Bearer " + creds.apiKey,
      "Content-Type": "application/json",
    };
  }

  function readError(status, body) {
    var msg = "provider error " + status;
    try {
      var j = JSON.parse(body);
      if (j && j.error && j.error.message) msg += ": " + j.error.message;
    } catch (e) {
      if (body) msg += ": " + String(body).slice(0, 180);
    }
    return C.redactText(msg, creds);
  }

  /* One POST to the Responses API (the wire the CLI defaults to,
   * docs/adr/0014): chat-shaped history translates onto `input` items and
   * the typed SSE events stream the answer / tool calls back. */
  function responsesOnce(msgs) {
    var url = C.joinApi(creds.baseUrl || C.DEFAULT_BASE, "/responses");
    abortCtl = new AbortController();
    return fetch(url, {
      method: "POST",
      headers: headers(),
      referrerPolicy: "no-referrer",
      signal: abortCtl.signal,
      body: JSON.stringify({
        model: creds.model || DEFAULT_MODEL,
        instructions: SYS,
        input: C.toResponsesInput(msgs),
        tools: TOOLS,
        tool_choice: "auto",
        stream: true,
        store: false,
      }),
    }).then(function (res) {
      if (!res.ok) {
        return res.text().then(function (body) {
          throw new Error(readError(res.status, body));
        });
      }
      if (!res.body || !res.body.getReader) {
        throw new Error("this browser cannot stream the provider response");
      }
      var reader = res.body.getReader();
      var dec = new TextDecoder();
      var parser = new C.SseParser();
      var turn = new C.ResponsesTurn();
      var el = line("assistant", "");
      function pump() {
        return reader.read().then(function (chunk) {
          if (chunk.done) {
            el.textContent = C.redactText(turn.content, creds);
            if (turn.error) throw new Error("provider error: " + turn.error);
            return { content: turn.content, tool_calls: turn.calls };
          }
          var events = parser.push(dec.decode(chunk.value, { stream: true }));
          events.forEach(function (ev) {
            if (!ev.json) return;
            if (turn.push(ev.json)) {
              el.textContent = C.redactText(turn.content, creds);
              transcript.scrollTop = transcript.scrollHeight;
            }
          });
          return pump();
        });
      }
      return pump();
    });
  }

  function runTurn(prompt) {
    if (!creds.apiKey) {
      sys("set OPENAI_API_KEY (hash, query, or /login). requests stay in this browser.");
      return Promise.resolve();
    }
    messages.push({ role: "user", content: prompt });
    setBusy(true);
    lastReply = "";
    var steps = 0;
    function loop() {
      if (steps++ >= STEP_LIMIT) {
        sys("step limit — interrupt or send another prompt");
        return;
      }
      return responsesOnce(messages).then(function (result) {
        if (result.tool_calls && result.tool_calls.length) {
          messages.push({
            role: "assistant",
            content: result.content || null,
            tool_calls: result.tool_calls,
          });
          result.tool_calls.forEach(function (tc) {
            var name = tc.function && tc.function.name;
            sys("tool " + (name || "?"));
            var out = execTool(name, tc.function && tc.function.arguments);
            messages.push({
              role: "tool",
              tool_call_id: tc.id,
              content: out,
            });
          });
          return loop();
        }
        lastReply = result.content || "";
        messages.push({ role: "assistant", content: lastReply });
      });
    }
    return loop()
      .catch(function (e) {
        if (e && e.name === "AbortError") sys("interrupted");
        else if (e && String(e.message || e).indexOf("Failed to fetch") >= 0)
          err("the provider refused the browser (CORS). use a CORS-open gateway.");
        else err(C.redactText(e && e.message ? e.message : String(e), creds));
      })
      .then(function () {
        abortCtl = null;
        setBusy(false);
        input.focus();
      });
  }

  function helpRows() {
    var rows = [
      "keys: enter submit · shift-enter newline · up/down history",
      "      / commands · esc cancel turn. key stays in this tab, encrypted.",
      "      pass #OPENAI_API_KEY=…&OPENAI_BASE_URL=… (hash, never sent to GitHub).",
    ];
    C.WEB_CMDS.forEach(function (c) {
      rows.push("  /" + (c[0] + "            ").slice(0, 12) + " " + c[1]);
    });
    rows.push("(esc hides this menu)");
    return rows;
  }

  function filterCmds(q) {
    var needle = (q || "").toLowerCase();
    return C.WEB_CMDS.filter(function (c) {
      return c[0].indexOf(needle) === 0 || c[0].indexOf(needle) >= 0;
    });
  }

  function paintPalette() {
    var v = input.value;
    if (!v || v.charAt(0) !== "/" || /\s/.test(v)) {
      if (overlayEl && !overlayEl.hidden && overlayEl.getAttribute("data-kind") === "help")
        return;
      hideOverlay();
      if (overlayEl) overlayEl.removeAttribute("data-kind");
      return;
    }
    var q = v.slice(1);
    var rows = filterCmds(q).map(function (c) {
      return "/" + c[0] + "  " + c[1];
    });
    if (!rows.length) {
      hideOverlay();
      return;
    }
    showOverlay(rows);
    if (overlayEl) overlayEl.setAttribute("data-kind", "palette");
  }

  function clearTranscript(keepBanner) {
    transcript.replaceChildren();
    if (keepBanner !== false) {
      var ban = document.createElement("div");
      ban.className = "term-banner";
      var bright = document.createElement("span");
      bright.className = "bright";
      bright.textContent = "tny";
      ban.appendChild(bright);
      ban.appendChild(text(" " + VERSION + " · Run /help for commands"));
      transcript.appendChild(ban);
    }
  }

  function newSession(clear) {
    messages = [];
    lastReply = "";
    if (clear) clearTranscript(true);
    sys("new session");
  }

  function runCommand(raw) {
    var body = raw.slice(1).replace(/^\s+/, "");
    var sp = body.indexOf(" ");
    var cmd = (sp < 0 ? body : body.slice(0, sp)).toLowerCase();
    var arg = sp < 0 ? "" : body.slice(sp + 1).replace(/^\s+/, "");
    hideOverlay();
    if (!cmd || cmd === "help") {
      showOverlay(helpRows());
      if (overlayEl) overlayEl.setAttribute("data-kind", "help");
      return;
    }
    if (cmd === "clear") {
      clearTranscript(true);
      return;
    }
    if (cmd === "new") {
      newSession(false);
      return;
    }
    if (cmd === "reset") {
      newSession(true);
      return;
    }
    if (cmd === "quit" || cmd === "exit") {
      sys("this is the browser demo — close the tab");
      return;
    }
    if (cmd === "login") {
      if (arg) {
        if (C.looksLikeSecretAssignment(arg)) applySecrets(C.parseEnvAssignments(arg));
        else applySecrets({ apiKey: arg });
      } else {
        secretMode = "key";
        sys("OPENAI_API_KEY (not echoed):");
        input.value = "";
        input.setAttribute("aria-label", "API key");
        paintBar();
      }
      return;
    }
    if (cmd === "logout") {
      wipeVault().then(function () {
        sys("logged out");
      });
      return;
    }
    if (cmd === "setup") {
      if (!arg) {
        sys("usage: /setup OPENAI_API_KEY=… OPENAI_BASE_URL=…");
        return;
      }
      applySecrets(C.parseEnvAssignments(arg));
      return;
    }
    if (cmd === "model") {
      if (arg) {
        creds.model = arg;
        persistCreds();
        paintStatus();
        sys("model set");
      } else sys("model: " + (creds.model || "default"));
      return;
    }
    if (cmd === "models") {
      if (!creds.apiKey) {
        sys("set a key first");
        return;
      }
      setBusy(true);
      fetch(C.joinApi(creds.baseUrl || C.DEFAULT_BASE, "/models"), {
        headers: headers(),
        referrerPolicy: "no-referrer",
      })
        .then(function (res) {
          return res.text().then(function (body) {
            if (!res.ok) throw new Error(readError(res.status, body));
            var j = JSON.parse(body);
            var ids = (j.data || []).map(function (m) {
              return m.id;
            }).filter(Boolean);
            sys(ids.length ? ids.slice(0, 24).join("\n") : "no models listed");
          });
        })
        .catch(function (e) {
          err(C.redactText(e && e.message ? e.message : String(e), creds));
        })
        .then(function () {
          setBusy(false);
        });
      return;
    }
    if (cmd === "status") {
      sys(
        "provider: openai\n" +
          "model: " +
          (creds.model || "default") +
          "\n" +
          "key: " +
          (creds.apiKey ? maskStatusKey() : "unset") +
          "\n" +
          "base: " +
          (creds.baseUrl ? C.obfuscateUrl(creds.baseUrl) : "default") +
          "\n" +
          "persist: " +
          persist +
          " (AES-GCM)\n" +
          "permission: yolo"
      );
      return;
    }
    if (cmd === "permissions") {
      sys("permission mode: yolo (browser demo)");
      return;
    }
    if (
      cmd === "resume" ||
      cmd === "continue" ||
      cmd === "mcp" ||
      cmd === "skills" ||
      cmd === "workspace" ||
      cmd === "provider" ||
      cmd === "backend" ||
      cmd === "compact"
    ) {
      sys("not available in the browser demo");
      return;
    }
    err("unknown command — /help lists them");
  }

  function maskStatusKey() {
    return creds.apiKey ? "set" : "unset";
  }

  function submit() {
    var raw = input.value.replace(/\s+$/, "");
    input.value = "";
    resizeInput();
    hideOverlay();
    if (secretMode === "key") {
      secretMode = null;
      input.setAttribute("aria-label", "Prompt");
      if (raw) applySecrets({ apiKey: raw });
      return;
    }
    if (!raw) return;
    if (C.looksLikeSecretAssignment(raw)) {
      applySecrets(C.parseEnvAssignments(raw));
      return;
    }
    history.push(raw);
    histIdx = -1;
    draft = "";
    if (raw.charAt(0) === "/") {
      runCommand(raw);
      return;
    }
    line("user", raw);
    runTurn(raw);
  }

  function interrupt() {
    if (busy && abortCtl) abortCtl.abort();
    hideOverlay();
    secretMode = null;
    input.setAttribute("aria-label", "Prompt");
  }

  input.addEventListener("keydown", function (e) {
    if (e.key === "Enter" && !e.shiftKey && !e.altKey) {
      e.preventDefault();
      if (busy) return;
      submit();
      return;
    }
    if (e.key === "Escape") {
      e.preventDefault();
      interrupt();
      return;
    }
    if (e.key === "c" && (e.ctrlKey || e.metaKey) && !e.altKey && !e.shiftKey) {
      if (busy) {
        e.preventDefault();
        interrupt();
      }
      return;
    }
    if ((e.key === "ArrowUp" || e.key === "ArrowDown") && !e.altKey) {
      if (input.selectionStart !== input.selectionEnd) return;
      if (e.key === "ArrowUp" && input.selectionStart > 0) return;
      if (e.key === "ArrowDown" && input.selectionStart < input.value.length) return;
      if (!history.length) return;
      e.preventDefault();
      if (histIdx === -1) draft = input.value;
      if (e.key === "ArrowUp") {
        if (histIdx === -1) histIdx = history.length - 1;
        else if (histIdx > 0) histIdx--;
      } else if (histIdx !== -1) {
        if (histIdx < history.length - 1) histIdx++;
        else {
          histIdx = -1;
          input.value = draft;
          resizeInput();
          return;
        }
      }
      input.value = history[histIdx] || "";
      resizeInput();
      input.selectionStart = input.selectionEnd = input.value.length;
    }
  });

  input.addEventListener("input", function () {
    resizeInput();
    paintPalette();
  });

  root.addEventListener("click", function () {
    input.focus();
  });

  var boot = window.__tnyBoot || {};
  window.__tnyBoot = null;
  if (boot.apiKey || boot.baseUrl || boot.model) applySecrets(boot, false);
  restoreVault().then(function () {
    paintStatus();
  });
  paintStatus();
  resizeInput();
})();
