// pre_node.js — node bootstrap for the CI wasm artifact (docs/adr/0017).
// Emscripten's synthetic environ hides the caller's env; the integration
// suite drives tny through HOME/OPENAI_*/TNY_*, so mirror process.env in
// before main. Secrets stay in the process env, never in the artifact.
Module['preRun'] = (Module['preRun'] || []).concat([function () {
  if (typeof process !== 'undefined' && process.env) {
    for (var k in process.env) ENV[k] = process.env[k];
  }
  // The TUI polls before reading stdin. NODERAWFS's synchronous read alone
  // cannot report readiness to tny_poll. Install this bridge only on the
  // first stdin poll, so CLI --stdin keeps its ordinary synchronous read.
  var input;
  var ended = false;
  Module['__tnyPollStdin'] = function () {
    if (!input) {
      input = process.stdin;
      var wake = function () {
        if (Module.__tny) Module.__tny.wake();
      };
      input.on('readable', wake);
      input.on('end', function () { ended = true; wake(); });
      input.on('error', function () { ended = true; wake(); });
      var read = FS.read;
      FS.read = function (stream, buffer, offset, length, position) {
        if (stream.nfd !== 0) return read(stream, buffer, offset, length, position);
        if (!length) return 0;
        var size = Math.min(length, input.readableLength);
        if (!size) {
          if (ended) return 0;
          throw new FS.ErrnoError(6); // EAGAIN in Emscripten's errno namespace
        }
        var bytes = input.read(size);
        buffer.set(bytes, offset);
        stream.position += bytes.length;
        return bytes.length;
      };
      input.read(0); // begin asynchronous reads; handlers never call into C
    }
    return input.readableLength > 0 || ended ? 1 : 0;
  };
}]);
