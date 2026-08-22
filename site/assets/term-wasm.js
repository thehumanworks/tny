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

  function intro() {
    term.write("\x1b[1mtny\x1b[0m — the real CLI, compiled to WebAssembly.\r\n");
    term.write("Runs entirely in this tab. Bring an OpenAI-compatible key;\r\n");
    term.write("it is validated at intake and sent only to your provider.\r\n");
    term.write("CORS note: api.openai.com refuses browser calls — use a\r\n");
    term.write("CORS-open gateway or a local http://127.0.0.1 server.\r\n\r\n");
  }

  function askCreds() {
    var key = C.sanitizeApiKey(boot.apiKey || "");
    var base = (boot.baseUrl || "").trim();
    if (key) return Promise.resolve({ key: key, base: base });
    intro();
    function askKey() {
      term.write("api key> ");
      return readLine(true).then(function (raw) {
        var k = C.sanitizeApiKey(raw);
        if (!k) {
          term.write("that key is empty after removing non-ISO-8859-1 junk (smart quotes, NBSP); paste it again\r\n");
          return askKey();
        }
        return k;
      });
    }
    return askKey().then(function (k) {
      term.write("base url [" + C.DEFAULT_BASE + "]> ");
      return readLine(false).then(function (b) {
        return { key: k, base: b.trim() };
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
          OPENAI_API_KEY: creds.key,
        };
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
