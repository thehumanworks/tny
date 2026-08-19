/* stream.c — nstream: plain fd or TLS (macOS SecureTransport).
 * Linux TLS (mbedTLS client) is a follow-up; plain HTTP works everywhere. */
#include "net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>

#ifdef __APPLE__
#include <Security/SecureTransport.h>
#include <dlfcn.h>
#endif

struct nstream {
    int fd;
#ifdef __APPLE__
    SSLContextRef ssl; /* NULL for plain */
#else
    void *ssl;
#endif
};

#ifdef __APPLE__
/* SecureTransport is dlopen'd on first TLS use: linking the Security +
 * CoreFoundation frameworks costs ~1.2 ms at every launch, which alone
 * would lose the startup race with fx (docs/size-and-speed.md). */
static struct {
    SSLContextRef (*create)(CFAllocatorRef, SSLProtocolSide, SSLConnectionType);
    OSStatus (*set_io)(SSLContextRef, SSLReadFunc, SSLWriteFunc);
    OSStatus (*set_conn)(SSLContextRef, SSLConnectionRef);
    OSStatus (*set_peer)(SSLContextRef, const char *, size_t);
    OSStatus (*set_min)(SSLContextRef, SSLProtocol);
    OSStatus (*handshake)(SSLContextRef);
    OSStatus (*read)(SSLContextRef, void *, size_t, size_t *);
    OSStatus (*write)(SSLContextRef, const void *, size_t, size_t *);
    OSStatus (*close)(SSLContextRef);
    void (*cf_release)(CFTypeRef);
} st_api;

static int st_api_load(void) {
    if (st_api.create) return 0;
    void *sec = dlopen("/System/Library/Frameworks/Security.framework/Security",
                       RTLD_LAZY | RTLD_LOCAL);
    void *cf = dlopen(
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
        RTLD_LAZY | RTLD_LOCAL);
    if (!sec || !cf) return -1;
    *(void **)&st_api.set_io     = dlsym(sec, "SSLSetIOFuncs");
    *(void **)&st_api.set_conn   = dlsym(sec, "SSLSetConnection");
    *(void **)&st_api.set_peer   = dlsym(sec, "SSLSetPeerDomainName");
    *(void **)&st_api.set_min    = dlsym(sec, "SSLSetProtocolVersionMin");
    *(void **)&st_api.handshake  = dlsym(sec, "SSLHandshake");
    *(void **)&st_api.read       = dlsym(sec, "SSLRead");
    *(void **)&st_api.write      = dlsym(sec, "SSLWrite");
    *(void **)&st_api.close      = dlsym(sec, "SSLClose");
    *(void **)&st_api.cf_release = dlsym(cf, "CFRelease");
    if (!st_api.set_io || !st_api.set_conn || !st_api.set_peer ||
        !st_api.set_min || !st_api.handshake || !st_api.read ||
        !st_api.write || !st_api.close || !st_api.cf_release)
        return -1;
    /* set last: it is the "loaded" flag */
    *(void **)&st_api.create = dlsym(sec, "SSLCreateContext");
    return st_api.create ? 0 : -1;
}

static OSStatus st_read(SSLConnectionRef conn, void *data, size_t *len) {
    int fd = (int)(intptr_t)conn;
    size_t want = *len, got = 0;
    while (got < want) {
        ssize_t n = read(fd, (char *)data + got, want - got);
        if (n > 0) { got += (size_t)n; continue; }
        if (n == 0) { *len = got; return errSSLClosedGraceful; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) { *len = got; return errSSLWouldBlock; }
        if (errno == EINTR) continue;
        *len = got;
        return errSecIO;
    }
    *len = got;
    return noErr;
}

static OSStatus st_write(SSLConnectionRef conn, const void *data, size_t *len) {
    int fd = (int)(intptr_t)conn;
    size_t want = *len, put = 0;
    while (put < want) {
        ssize_t n = write(fd, (const char *)data + put, want - put);
        if (n > 0) { put += (size_t)n; continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) { *len = put; return errSSLWouldBlock; }
        if (errno == EINTR) continue;
        *len = put;
        return errSecIO;
    }
    *len = put;
    return noErr;
}
#endif

nstream *nstream_from_fd(int fd) {
    nstream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->fd = fd;
    return s;
}

nstream *nstream_connect(const char *host, int port, bool tls,
                         int timeout_ms, char *err, size_t errlen) {
    int fd = tcp_connect(host, port, timeout_ms);
    if (fd < 0) {
        snprintf(err, errlen, "connect %s:%d failed", host, port);
        return NULL;
    }
    nstream *s = nstream_from_fd(fd);
    if (!s) { close(fd); return NULL; }
    if (!tls) return s;
#ifdef __APPLE__
    if (st_api_load() != 0) {
        snprintf(err, errlen, "TLS unavailable: Security.framework failed to load");
        nstream_close(s);
        return NULL;
    }
    s->ssl = st_api.create(NULL, kSSLClientSide, kSSLStreamType);
    if (!s->ssl) { snprintf(err, errlen, "TLS context failed"); nstream_close(s); return NULL; }
    st_api.set_io(s->ssl, st_read, st_write);
    st_api.set_conn(s->ssl, (SSLConnectionRef)(intptr_t)fd);
    st_api.set_peer(s->ssl, host, strlen(host));
    st_api.set_min(s->ssl, kTLSProtocol12);
    OSStatus rc;
    int64_t deadline = now_ms() + timeout_ms;
    while ((rc = st_api.handshake(s->ssl)) == errSSLWouldBlock) {
        if (now_ms() > deadline) { rc = errSecIO; break; }
        struct pollfd pf = {fd, POLLIN | POLLOUT, 0};
        poll(&pf, 1, 100);
    }
    if (rc != noErr) {
        snprintf(err, errlen, "TLS handshake with %s failed (%d)", host, (int)rc);
        nstream_close(s);
        return NULL;
    }
    return s;
#else
    snprintf(err, errlen,
             "https not built on this platform yet; use an http:// base URL");
    nstream_close(s);
    return NULL;
#endif
}

ssize_t nstream_read(nstream *s, void *buf, size_t cap) {
#ifdef __APPLE__
    if (s->ssl) {
        size_t got = 0;
        OSStatus rc = st_api.read(s->ssl, buf, cap, &got);
        if (got > 0) return (ssize_t)got;
        if (rc == errSSLWouldBlock) return -2;
        if (rc == errSSLClosedGraceful || rc == errSSLClosedNoNotify) return 0;
        if (rc == noErr) return 0;
        return -1;
    }
#endif
    ssize_t n = read(s->fd, buf, cap);
    if (n >= 0) return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
    if (errno == EINTR) return -2;
    return -1;
}

int nstream_write_all(nstream *s, const void *data, size_t len) {
#ifdef __APPLE__
    if (s->ssl) {
        size_t off = 0;
        while (off < len) {
            size_t put = 0;
            OSStatus rc = st_api.write(s->ssl, (const char *)data + off, len - off, &put);
            off += put;
            if (rc == noErr) continue;
            if (rc == errSSLWouldBlock) {
                struct pollfd pf = {s->fd, POLLOUT, 0};
                poll(&pf, 1, 5000);
                continue;
            }
            return -1;
        }
        return 0;
    }
#endif
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(s->fd, (const char *)data + off, len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = {s->fd, POLLOUT, 0};
            poll(&pf, 1, 5000);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

int nstream_fd(nstream *s) { return s->fd; }

void nstream_close(nstream *s) {
    if (!s) return;
#ifdef __APPLE__
    if (s->ssl) {
        st_api.close(s->ssl);
        st_api.cf_release(s->ssl);
    }
#endif
    if (s->fd >= 0) close(s->fd);
    free(s);
}
