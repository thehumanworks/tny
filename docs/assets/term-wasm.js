/* term-wasm.js — boots the real tny CLI (compiled to WebAssembly, the same
 * sources and CI suite as the native binary — docs/adr/0017) into an
 * xterm.js terminal. BYOK, client-side only: the key comes from the URL
 * hash (already captured by term-core.js takeSecretsFromLocation before
 * any other script ran) or is pasted at the prompt below, passes through
 * sanitizeApiKey at intake, lives in this tab's memory, and is sent only
 * to the provider base URL. No agent loop lives in this file: the loop is
 * the wasm binary. */
(function () {
  var C = window.tnyTermCore;
  var boot = window.__tnyBoot || {};
  var mount = document.querySelector("[data-term-xterm]");
  if (!C || !mount || !window.Terminal) return;

  var term = new window.Terminal({
    fontSize: 12.5,
    fontFamily: '"SF Mono", SFMono-Regular, Menlo, Consolas, "Liberation Mono", monospace',
    cursorBlink: true,
    convertEol: false,
    scrollback: 4000,
    theme: {
      background: "#00000000",
      foreground: "#d4d4d4",
      cursor: "#d4d4d4",
    },
    allowTransparency: true,
  });
  term.open(mount);

  /* Fit to the mount. xterm defaults to 80 columns; a phone-width
   * .term-wrap is ~40, and overflow:hidden was clipping the welcome
   * lines down the right edge. */
  function cellSize() {
    var core = term._core;
    if (core && core._renderService && core._renderService.dimensions) {
      var d = core._renderService.dimensions;
      var cell = (d.css && d.css.cell) || d.actualCell;
      if (cell && cell.width > 0 && cell.height > 0) {
        return { width: cell.width, height: cell.height };
      }
    }
    var probe = document.createElement("span");
    probe.textContent = "MMMMMMMM";
    probe.style.cssText =
      "position:absolute;visibility:hidden;white-space:pre;font-family:" +
      term.options.fontFamily +
      ";font-size:" +
      term.options.fontSize +
      "px";
    mount.appendChild(probe);
    var r = probe.getBoundingClientRect();
    mount.removeChild(probe);
    return {
      width: Math.max(1, r.width / 8),
      height: Math.max(term.options.fontSize * 1.2, r.height),
    };
  }

  function fit() {
    var cell = cellSize();
    var sb = 0;
    if (term.element) {
      var vp = term.element.querySelector(".xterm-viewport");
      if (vp) sb = Math.max(0, vp.offsetWidth - vp.clientWidth);
    }
    var geo = C.proposeTermGeometry(
      mount.clientWidth - sb,
      mount.clientHeight,
      cell.width,
      cell.height
    );
    if (geo.cols !== term.cols || geo.rows !== term.rows) {
      term.resize(geo.cols, geo.rows);
    }
    mount.setAttribute("data-term-cols", String(term.cols));
    mount.setAttribute("data-term-rows", String(term.rows));
  }

  function syncViewport() {
    var vv = window.visualViewport;
    if (vv) {
      document.documentElement.style.setProperty("--vv-height", vv.height + "px");
      document.documentElement.classList.toggle(
        "kb-open",
        (window.innerHeight || 0) - vv.height > 120
      );
    }
    fit();
  }

  var fitQ = 0;
  function requestFit() {
    if (fitQ) return;
    fitQ = window.requestAnimationFrame(function () {
      fitQ = 0;
      syncViewport();
    });
  }

  syncViewport();
  if (window.ResizeObserver) {
    new window.ResizeObserver(requestFit).observe(mount);
  }
  window.addEventListener("resize", requestFit);
  if (window.visualViewport) {
    window.visualViewport.addEventListener("resize", requestFit);
    window.visualViewport.addEventListener("scroll", requestFit);
  }
  if (term.element) {
    term.element.addEventListener("focusin", function () {
      var box = document.getElementById("tny-term");
      if (box && box.scrollIntoView) {
        box.scrollIntoView({ block: "nearest", inline: "nearest" });
      }
      requestFit();
    });
  }

  var stdinQ = [];
  var enc = new TextEncoder();
  var started = false;
  var lineSink = null;

  term.onData(function (d) {
    if (started) {
      var bytes = enc.encode(d);
      for (var i = 0; i < bytes.length; i++) stdinQ.push(bytes[i]);
      return;
    }
    if (lineSink) lineSink(d);
  });

  /* Minimal line reader for the pre-launch prompts (masked for the key). */
  function readLine(mask) {
    return new Promise(function (res) {
      var acc = "";
      lineSink = function (d) {
        for (var i = 0; i < d.length; i++) {
          var ch = d[i];
          if (ch === "\r" || ch === "\n") {
            lineSink = null;
            term.write("\r\n");
            res(acc);
            return;
          }
          if (ch === "\x7f" || ch === "\b") {
            if (acc.length) {
              acc = acc.slice(0, -1);
              term.write("\b \b");
            }
            continue;
          }
          if (ch < " " && ch !== "\t") continue;
          acc += ch;
          term.write(mask ? "*" : ch);
        }
      };
    });
  }

  function writeln(text) {
    var lines = C.wrapToCols(text, term.cols);
    for (var i = 0; i < lines.length; i++) term.write(lines[i] + "\r\n");
  }

  function writeTitle() {
    var title = C.wrapToCols("tny — the real CLI, compiled to WebAssembly.", term.cols);
    if (!title.length) return;
    var first = title[0];
    term.write(
      first.indexOf("tny") === 0
        ? "\x1b[1mtny\x1b[0m" + first.slice(3) + "\r\n"
        : first + "\r\n"
    );
    for (var t = 1; t < title.length; t++) term.write(title[t] + "\r\n");
  }

  function intro() {
    writeTitle();
    /* A 320×568 phone has ~12 visible rows after chrome. Keep the
     * prompt on screen instead of painting a desktop-length banner. */
    if (term.cols < 42 || term.rows < 16) {
      writeln("Runs in this tab. Bring an OpenAI-compatible key, or leave empty and use /provider setup.");
      writeln("api.openai.com blocks browser CORS — use a gateway or 127.0.0.1.");
      term.write("\r\n");
      return;
    }
    writeln("Runs entirely in this tab. Bring an OpenAI-compatible key;");
    writeln("it is validated at intake and sent only to your provider.");
    writeln("Any provider works: pass NAME_BASE_URL + NAME_API_KEY in the");
    writeln("URL hash, or run /provider setup inside the terminal.");
    writeln("CORS note: api.openai.com refuses browser calls — use a");
    writeln("CORS-open gateway or a local http://127.0.0.1 server.");
    term.write("\r\n");
  }

  /* Named-provider pairs from the hash (docs/adr/0018): keys sanitized at
   * intake, base urls scheme-checked; a bad value is reported and dropped
   * rather than shipped to fetch() where it would throw later. */
  function namedEnv() {
    var env = {};
    var pairs = boot.env || {};
    for (var k in pairs) {
      var v = pairs[k];
      if (/_API_KEY$/.test(k)) {
        v = C.sanitizeApiKey(v);
        if (!v) {
          term.write("ignoring " + k + ": empty after removing non-ISO-8859-1 junk\r\n");
          continue;
        }
      } else if (/_BASE_URL$/.test(k)) {
        v = C.sanitizeBaseUrl(v);
        if (!v) {
          term.write("ignoring " + k + ": not an http(s) URL\r\n");
          continue;
        }
      }
      env[k] = v;
    }
    return env;
  }

  function askCreds() {
    var key = C.sanitizeApiKey(boot.apiKey || "");
    var base = (boot.baseUrl || "").trim();
    var env = namedEnv();
    if (key || Object.keys(env).length)
      return Promise.resolve({ key: key, base: base, env: env });
    intro();
    function askKey() {
      var hint = term.cols < 48
        ? "api key (empty skips — /provider setup)> "
        : "api key (empty to start without one — /provider setup adds any provider)> ";
      term.write(hint);
      return readLine(true).then(function (raw) {
        if (!raw.trim()) return "";
        var k = C.sanitizeApiKey(raw);
        if (!k) {
          term.write("that key is empty after removing non-ISO-8859-1 junk (smart quotes, NBSP); paste it again\r\n");
          return askKey();
        }
        return k;
      });
    }
    return askKey().then(function (k) {
      if (!k) return { key: "", base: "", env: env };
      term.write("base url [" + C.DEFAULT_BASE + "]> ");
      return readLine(false).then(function (b) {
        return { key: k, base: b.trim(), env: env };
      });
    });
  }

  function start(creds) {
    started = true;
    term.write("loading tny.wasm…\r\n");
    import("./wasm/tny-web.mjs")
      .then(function (mod) {
        var env = {
          HOME: "/home/web_user",
          TERM: "xterm-256color",
        };
        for (var k in (creds.env || {})) env[k] = creds.env[k];
        if (creds.key) env.OPENAI_API_KEY = creds.key;
        if (creds.base) env.OPENAI_BASE_URL = creds.base;
        return mod.default({
          tnyEnv: env,
          __tnyStdin: stdinQ,
          tnyWinsize: [term.cols, term.rows],
          tnyOut: function (bytes) { term.write(bytes); },
          onExit: function (code) {
            started = false;
            // after the binary's own final erase/restore output flushes
            // (stdout batches per microtask), not before it
            setTimeout(function () {
              term.write("\r\n\x1b[2m[tny exited " + code +
                         " — reload the page to restart]\x1b[0m\r\n");
            }, 50);
          },
        });
      })
      .then(function (Module) {
        term.onResize(function (sz) {
          Module.tnyWinsize = [sz.cols, sz.rows];
          if (Module._tny_wasm_winch) Module._tny_wasm_winch();
        });
        term.focus();
        Module.callMain([]);
      })
      .catch(function (err) {
        started = false;
        term.write("\r\nfailed to start tny.wasm: " + String(err) + "\r\n");
      });
  }

  askCreds().then(start);
})();
