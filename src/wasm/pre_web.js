// pre_web.js — browser bootstrap seam for tny-web.mjs (docs/adr/0017).
// The page (site/assets/term-wasm.js) supplies, before callMain:
//   Module.tnyEnv     — {HOME, OPENAI_API_KEY, OPENAI_BASE_URL, …} from the
//                       hash/vault; validated by sanitizeApiKey at intake.
//   Module.__tnyStdin — byte queue fed by xterm.js onData; fd 0 pulls from
//                       it and tny_poll reports it ready when non-empty.
//   Module.tnyWinsize — [cols, rows], kept fresh by the resize handler
//                       (read by tui_size; _tny_wasm_winch raises SIGWINCH).
//   Module.tnyOut     — receives Uint8Array chunks of stdout+stderr.
// Output bytes batch per microtask so escape sequences arrive whole; JS
// never calls into suspended wasm (the Asyncify re-entry contract).
Module['preRun'] = (Module['preRun'] || []).concat([function () {
  var env = Module['tnyEnv'] || {};
  for (var k in env) ENV[k] = env[k];
  if (!ENV['HOME']) ENV['HOME'] = '/home/web_user';
  var stdin = (Module['__tnyStdin'] = Module['__tnyStdin'] || []);
  var outBuf = [];
  var flushing = false;
  function flush() {
    flushing = false;
    if (outBuf.length && Module['tnyOut']) {
      Module['tnyOut'](new Uint8Array(outBuf));
      outBuf.length = 0;
    }
  }
  function sink(c) {
    if (c === null) { flush(); return; }
    outBuf.push(c & 0xFF);
    if (!flushing) {
      flushing = true;
      Promise.resolve().then(flush);
    }
  }
  FS.init(function () {
    // undefined = no data right now (EAGAIN), never null (EOF): the page
    // terminal has no end-of-input.
    return stdin.length ? stdin.shift() : undefined;
  }, sink, sink);
}]);
