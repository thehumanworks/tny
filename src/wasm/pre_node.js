// pre_node.js — node bootstrap for the CI wasm artifact (docs/adr/0017).
// Emscripten's synthetic environ hides the caller's env; the integration
// suite drives tny through HOME/OPENAI_*/TNY_*, so mirror process.env in
// before main. Secrets stay in the process env, never in the artifact.
Module['preRun'] = (Module['preRun'] || []).concat([function () {
  if (typeof process !== 'undefined' && process.env) {
    for (var k in process.env) ENV[k] = process.env[k];
  }
}]);
