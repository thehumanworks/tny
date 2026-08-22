// pre_web.js — browser bootstrap seam for tny-web.mjs (docs/adr/0017).
// The page (site/assets/term-wasm.js) supplies, before callMain:
//   Module.tnyEnv    — {HOME, OPENAI_API_KEY, OPENAI_BASE_URL, …} from the
//                      key vault; validated by sanitizeApiKey at intake.
//   Module.__tnyStdin — byte queue fed by xterm.js onData; fd 0 pulls from
//                      it and tny_poll reports it ready when non-empty.
//   Module.tnyWinsize — [cols, rows], kept fresh by the resize handler.
Module['preRun'] = (Module['preRun'] || []).concat([function () {
  var env = Module['tnyEnv'] || {};
  for (var k in env) ENV[k] = env[k];
  if (!ENV['HOME']) ENV['HOME'] = '/home/web_user';
  var stdin = (Module['__tnyStdin'] = Module['__tnyStdin'] || []);
  FS.init(function () {
    return stdin.length ? stdin.shift() : null;
  }, null, null);
}]);
