/* net_wasm.c — the whole net seam on WebAssembly (docs/adr/0017).
 *
 * Replaces tcp.c/stream.c/http1.c/ws.c wholesale in the wasm build:
 * http_conn rides fetch() + ReadableStream, ws_conn rides the browser/node
 * WebSocket, and tny_poll waits on the pseudo-fd registry with Asyncify.
 *
 * Re-entry contract (the load-bearing part): JS never calls into C. Every
 * event handler only appends bytes/messages to per-fd queues held in JS and
 * resolves the poll waker; C pulls when awake. The only suspension points
 * are tny_poll, the header/handshake waits below, and nothing else — so no
 * callback can re-enter a suspended export.
 *
 * Pseudo-fds start at 64 and never collide with real MEMFS/NODERAWFS fds in
 * practice (the CLI opens a handful of files); they exist only so the
 * backend pollfds/dispatch contract carries over unchanged. */
/* clang-format must not tokenize JavaScript inside EM_JS/EM_ASYNC_JS bodies:
 * it splits `=>` and `===`, producing invalid generated JavaScript. */
// clang-format off
#ifdef __EMSCRIPTEN__

#include "net/net.h"
#include "util/tny_wake.h"
#include "util/tny_poll.h"

#include <emscripten.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

/* ---- shared JS registry ----
 * Module.__tny = { fds: Map(fd -> entry), next, wakers, wake() }
 * entry = { q: [Uint8Array], msgs: [string], done, err, status, hdrs,
 *           hdrsReady, ws, ctl }                                        */

EM_JS(int, js_net_alloc, (void), {
  const T = Module.__tny || (Module.__tny = {fds: new Map(), next: 64,
    wakers: [],
    wake() { const w = this.wakers; this.wakers = []; w.forEach((f) => f()); }});
  const fd = T.next++;
  T.fds.set(fd, {q: [], msgs: [], done: false, err: null, status: 0,
                 hdrs: null, hdrsReady: false, hdrsTaken: false,
                 ws: null, ctl: null});
  return fd;
});

EM_JS(void, js_net_free, (int fd), {
  const T = Module.__tny;
  if (!T) return;
  const e = T.fds.get(fd);
  if (e) {
    if (e.ctl) { try { e.ctl.abort(); } catch (_) {} }
    if (e.ws) { try { e.ws.close(); } catch (_) {} }
    T.fds.delete(fd);
  }
});

/* 1 when a read on fd would make progress (bytes, messages, EOF or error
 * to report, or unconsumed response headers). Stdin (fd 0) is ready when
 * the page bootstrap queued keystrokes in Module.__tnyStdin. */
EM_JS(int, js_fd_ready, (int fd), {
  if (fd === 0) return (Module.__tnyStdin && Module.__tnyStdin.length) ? 1 : 0;
  const e = Module.__tny && Module.__tny.fds.get(fd);
  if (!e) return 0;
  return (e.q.length || e.msgs.length || e.done || e.err ||
          (e.hdrsReady && !e.hdrsTaken)) ? 1 : 0;
});

/* Suspend until any registered fd event fires or timeout_ms passes.
 * 1 = woken by an event, 0 = timeout. */
EM_ASYNC_JS(int, js_wait_any, (int timeout_ms), {
  const T = Module.__tny || (Module.__tny = {fds: new Map(), next: 64,
    wakers: [],
    wake() { const w = this.wakers; this.wakers = []; w.forEach((f) => f()); }});
  return await new Promise((res) => {
    const t = setTimeout(() => {
      const i = T.wakers.indexOf(hit);
      if (i >= 0) T.wakers.splice(i, 1);
      res(0);
    }, timeout_ms);
    function hit() { clearTimeout(t); res(1); }
    T.wakers.push(hit);
  });
});

int tny_poll(struct pollfd *fds, nfds_t n, int timeout_ms) {
  int64_t deadline = now_ms() + timeout_ms;
  for (;;) {
    int ready = 0;
    for (nfds_t i = 0; i < n; i++) {
      fds[i].revents = 0;
      if (js_fd_ready(fds[i].fd)) {
        fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
        if (!fds[i].revents) fds[i].revents = POLLIN;
        ready++;
      }
    }
    if (ready || timeout_ms == 0) return ready;
    int left = (int)(deadline - now_ms());
    if (left <= 0) return 0;
    js_wait_any(left);
  }
}

/* Public libtny is native-only. Shared runtime code still references the wake
 * surface, so wasm supplies a clean unavailable stub at the existing poll
 * seam rather than compiling POSIX pipe code into the browser target. */
int tny_wake_init(tny_wake *wake) {
  if (wake) { wake->read_fd = -1; wake->write_fd = -1; }
  return -1;
}
void tny_wake_close(tny_wake *wake) {
  if (wake) { wake->read_fd = -1; wake->write_fd = -1; }
}
int tny_wake_fd(const tny_wake *wake) { (void)wake; return -1; }
void tny_wake_signal(tny_wake *wake) { (void)wake; }
void tny_wake_drain(tny_wake *wake) { (void)wake; }

int set_nonblock(int fd, bool nb) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -1;
  return fcntl(fd, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

/* Resize hook for the page: raising SIGWINCH only sets the TUI's flag, so
 * it is safe to call from JS even while the wasm is Asyncify-suspended. */
EMSCRIPTEN_KEEPALIVE
void tny_wasm_winch(void) { raise(SIGWINCH); }

/* Native-only entry points that shared code still links against. */
int tcp_connect(const char *host, int port, int timeout_ms) {
  (void)host; (void)port; (void)timeout_ms;
  return -1;
}
int unix_connect(const char *path) { (void)path; return -1; }
int unix_listen(const char *path) { (void)path; return -1; }

/* ---- HTTP over fetch ---- */

struct http_conn {
  int fd;             /* pseudo-fd in the JS registry */
  char origin[512];   /* scheme://host[:port], no path */
  char prefix[1024];
  /* response headers, same shape as the native parser exposes */
  char hdr_names[64][64];
  char hdr_values[64][512];
  int n_hdrs;
  int status;
  bool have_status;
};

const char *http_prefix(http_conn *c) { return c->prefix; }
int http_fd(http_conn *c) { return c->fd; }

http_conn *http_open(const char *base_url, char *err, size_t errlen) {
  url_parts u;
  if (url_parse(base_url, &u) != 0) {
    snprintf(err, errlen, "bad base URL: %s", base_url);
    return NULL;
  }
  if (strcmp(u.scheme, "http") != 0 && strcmp(u.scheme, "https") != 0) {
    snprintf(err, errlen, "unsupported scheme %s", u.scheme);
    return NULL;
  }
  http_conn *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->fd = js_net_alloc();
  snprintf(c->origin, sizeof c->origin, "%s://%s:%d", u.scheme, u.host, u.port);
  snprintf(c->prefix, sizeof c->prefix, "%s", u.path);
  size_t pl = strlen(c->prefix);
  if (pl && c->prefix[pl - 1] == '/') c->prefix[pl - 1] = 0;
  if (strcmp(c->prefix, "/") == 0) c->prefix[0] = 0;
  return c;
}

http_conn *http_from_fd(int fd) { (void)fd; return NULL; }

/* Start a fetch; response bytes stream into the fd queue. hdrs_joined is
 * "Name: value\n"-joined; names fetch() refuses to set are skipped one by
 * one so the rest still ride. */
EM_JS(void, js_http_start, (int fd, const char *method, const char *url,
                            const char *hdrs_joined, const char *body,
                            int body_len), {
  const T = Module.__tny;
  const e = T.fds.get(fd);
  e.q = []; e.done = false; e.err = null; e.status = 0;
  e.hdrs = null; e.hdrsReady = false; e.hdrsTaken = false;
  const h = new Headers();
  for (const line of UTF8ToString(hdrs_joined).split("\n")) {
    if (!line) continue;
    const i = line.indexOf(":");
    if (i < 0) continue;
    try { h.set(line.slice(0, i).trim(), line.slice(i + 1).trim()); }
    catch (_) { /* forbidden header name in this runtime; fetch owns it */ }
  }
  const opts = {method: UTF8ToString(method), headers: h};
  if (body_len >= 0)
    opts.body = HEAPU8.slice(body, body + body_len);
  e.ctl = new AbortController();
  opts.signal = e.ctl.signal;
  fetch(UTF8ToString(url), opts).then(async (r) => {
    e.status = r.status;
    const lines = [];
    r.headers.forEach((v, k) => lines.push(k + ": " + v));
    e.hdrs = lines.join("\n");
    e.hdrsReady = true;
    T.wake();
    if (!r.body) { e.done = true; T.wake(); return; }
    const rd = r.body.getReader();
    for (;;) {
      let value, done;
      try { ({value, done} = await rd.read()); }
      catch (err) { e.err = String(err); break; }
      if (value && value.length) { e.q.push(value); T.wake(); }
      if (done) break;
    }
    e.done = true;
    T.wake();
  }).catch((err) => {
    e.err = String(err);
    e.done = true;
    T.wake();
  });
});

/* Copy queued body bytes out. >0 bytes, 0 clean EOF, -1 error, -2 empty. */
EM_JS(int, js_http_read, (int fd, char *buf, int cap), {
  const e = Module.__tny.fds.get(fd);
  if (!e) return -1;
  let got = 0;
  while (got < cap && e.q.length) {
    let chunk = e.q[0];
    const take = Math.min(cap - got, chunk.length);
    HEAPU8.set(chunk.subarray(0, take), buf + got);
    got += take;
    if (take === chunk.length) e.q.shift();
    else e.q[0] = chunk.subarray(take);
  }
  if (got > 0) return got;
  if (e.err && !e.hdrsReady) return -1; /* failed before any response */
  if (e.done) return e.err ? -1 : 0;
  return -2;
});

/* Report the status once ready — and mark the headers consumed, so the
 * pseudo-fd stops reporting ready for them (a ready-forever fd would make
 * tny_poll spin without yielding, starving the very event loop the fetch
 * needs to deliver body bytes). */
EM_JS(int, js_http_status, (int fd), {
  const e = Module.__tny.fds.get(fd);
  if (!e) return -1;
  if (e.err && !e.hdrsReady) return -1;
  if (!e.hdrsReady) return -2;
  e.hdrsTaken = true;
  return e.status;
});

/* Response header lines into buf ("k: v\n"-joined); returns byte length. */
EM_JS(int, js_http_headers, (int fd, char *buf, int cap), {
  const e = Module.__tny.fds.get(fd);
  if (!e || e.hdrs === null) return 0;
  return stringToUTF8(e.hdrs, buf, cap), lengthBytesUTF8(e.hdrs.slice(0, cap));
});

int http_request(http_conn *c, const char *method, const char *path,
                 const char **headers, const char *body, size_t body_len) {
  buf_t url, hj;
  buf_init(&url);
  buf_init(&hj);
  buf_appendf(&url, "%s%s", c->origin, path);
  if (headers)
    for (int i = 0; headers[i]; i++) buf_appendf(&hj, "%s\n", headers[i]);
  c->n_hdrs = 0;
  c->have_status = false;
  js_http_start(c->fd, method, url.data, hj.data ? hj.data : "",
                body ? body : "", body ? (int)body_len : -1);
  buf_free(&url);
  if (hj.data) secure_zero(hj.data, hj.cap);
  buf_free(&hj);
  return 0;
}

static void parse_headers(http_conn *c) {
  char tmp[16384];
  int n = js_http_headers(c->fd, tmp, sizeof tmp);
  (void)n;
  tmp[sizeof tmp - 1] = 0;
  c->n_hdrs = 0;
  char *line = tmp;
  while (line && *line && c->n_hdrs < 64) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    char *colon = strchr(line, ':');
    if (colon) {
      *colon = 0;
      const char *v = colon + 1;
      while (*v == ' ') v++;
      snprintf(c->hdr_names[c->n_hdrs], sizeof c->hdr_names[0], "%s", line);
      snprintf(c->hdr_values[c->n_hdrs], sizeof c->hdr_values[0], "%s", v);
      c->n_hdrs++;
    }
    line = nl ? nl + 1 : NULL;
  }
}

int http_read_response(http_conn *c, int timeout_ms) {
  int64_t deadline = now_ms() + timeout_ms;
  for (;;) {
    int st = js_http_status(c->fd);
    if (st == -1) return -1;
    if (st >= 0) {
      c->status = st;
      c->have_status = true;
      parse_headers(c);
      return st;
    }
    int left = (int)(deadline - now_ms());
    if (timeout_ms == 0 || left <= 0) return -2;
    js_wait_any(left);
  }
}

const char *http_header(http_conn *c, const char *name) {
  for (int i = 0; i < c->n_hdrs; i++)
    if (strcasecmp(c->hdr_names[i], name) == 0) return c->hdr_values[i];
  return NULL;
}

ssize_t http_body_read(http_conn *c, char *out, size_t cap) {
  int n = js_http_read(c->fd, out, cap > 0x40000000 ? 0x40000000 : (int)cap);
  return n;
}

void http_close(http_conn *c) {
  if (!c) return;
  js_net_free(c->fd);
  free(c);
}

/* ---- WebSocket ---- */

struct ws_conn {
  int fd;
  bool dead;
};

/* Open a WebSocket; complete text messages queue in e.msgs. Returns 0 on
 * open, -1 on failure. The browser API cannot attach an Authorization
 * header, so a required bearer is refused up front (docs/adr/0017). */
EM_ASYNC_JS(int, js_ws_open, (int fd, const char *url, int timeout_ms), {
  const T = Module.__tny;
  const e = T.fds.get(fd);
  const WS = (typeof WebSocket !== "undefined")
      ? WebSocket
      : (typeof globalThis !== "undefined" ? globalThis.WebSocket : undefined);
  if (!WS) return -1;
  return await new Promise((res) => {
    let ws;
    try { ws = new WS(UTF8ToString(url)); } catch (_) { res(-1); return; }
    const t = setTimeout(() => { try { ws.close(); } catch (_) {} res(-1); },
                         timeout_ms);
    ws.onopen = () => { clearTimeout(t); e.ws = ws; res(0); };
    ws.onmessage = (ev) => {
      if (typeof ev.data === "string") { e.msgs.push(ev.data); T.wake(); }
    };
    ws.onerror = () => {};
    ws.onclose = () => {
      clearTimeout(t);
      e.done = true;
      T.wake();
      if (!e.ws) res(-1);
    };
  });
});

EM_JS(int, js_ws_send, (int fd, const char *data, int len), {
  const e = Module.__tny.fds.get(fd);
  if (!e || !e.ws || e.ws.readyState !== 1) return -1;
  try { e.ws.send(UTF8ToString(data, len)); } catch (_) { return -1; }
  return 0;
});

/* Pop one queued message into malloc'd heap memory; returns ptr or 0.
 * len_out receives the byte length. */
EM_JS(char *, js_ws_next, (int fd, int *len_out), {
  const e = Module.__tny.fds.get(fd);
  if (!e || !e.msgs.length) return 0;
  const s = e.msgs.shift();
  const n = lengthBytesUTF8(s);
  const p = _malloc(n + 1);
  stringToUTF8(s, p, n + 1);
  HEAP32[len_out >> 2] = n;
  return p;
});

EM_JS(int, js_ws_dead, (int fd), {
  const e = Module.__tny.fds.get(fd);
  return (!e || (e.done && !e.msgs.length)) ? 1 : 0;
});

ws_conn *ws_connect(const char *url, const char *bearer, int timeout_ms,
                    char *err, size_t errlen) {
  url_parts u;
  if (url_parse(url, &u) != 0) {
    snprintf(err, errlen, "bad ws URL: %s", url);
    return NULL;
  }
  if (strcmp(u.scheme, "ws") != 0 && strcmp(u.scheme, "wss") != 0) {
    snprintf(err, errlen, "unsupported scheme %s (wasm has no unix sockets)",
             u.scheme);
    return NULL;
  }
  if (bearer && *bearer) {
    snprintf(err, errlen,
             "wasm WebSocket cannot send an Authorization header; run the "
             "host without token auth on loopback");
    return NULL;
  }
  ws_conn *w = calloc(1, sizeof *w);
  if (!w) return NULL;
  w->fd = js_net_alloc();
  if (js_ws_open(w->fd, url, timeout_ms) != 0) {
    snprintf(err, errlen, "ws connect %s failed", url);
    js_net_free(w->fd);
    free(w);
    return NULL;
  }
  return w;
}

int ws_send_text(ws_conn *w, const char *data, size_t len) {
  if (w->dead) return -1;
  return js_ws_send(w->fd, data, (int)len);
}

int ws_fd(ws_conn *w) { return w->fd; }

bool ws_want_write(ws_conn *w) { (void)w; return false; }

int ws_pump(ws_conn *w, ws_msg_cb cb, void *ud) {
  if (w->dead) return -1;
  for (;;) {
    int len = 0;
    char *msg = js_ws_next(w->fd, &len);
    if (!msg) break;
    if (cb) cb(msg, (size_t)len, ud);
    free(msg);
  }
  if (js_ws_dead(w->fd)) { w->dead = true; return -1; }
  return 0;
}

void ws_close(ws_conn *w) {
  if (!w) return;
  js_net_free(w->fd);
  free(w);
}

#endif /* __EMSCRIPTEN__ */
// clang-format on
