/* stream.c — nstream: plain fd or TLS.
 * macOS: SecureTransport, dlopen'd at first TLS use.
 * Linux: system OpenSSL (libssl.so.3 / .so.1.1), dlopen'd at first TLS use
 *        (docs/adr/0007). Never linked, never vendored, never static. */
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
#elif defined(__linux__)
#include <dlfcn.h>
#include <limits.h>
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

#ifdef __linux__
/* System OpenSSL is dlopen'd on first TLS use, mirroring the macOS
 * SecureTransport shim: no link-time libssl dependency, zero cost for
 * plain-http and non-TLS commands, and the binary stays small. In a musl
 * static publish build dlopen returns NULL and https fails cleanly. */
typedef struct ossl_ctx ossl_ctx;   /* SSL_CTX */
typedef struct ossl_ssl ossl_ssl;   /* SSL */

/* Stable OpenSSL >= 1.1.0 ABI constants (ssl.h / tls1.h). */
#define OSSL_VERIFY_PEER            1
#define OSSL_CTRL_SET_TLSEXT_HOST   55   /* SSL_CTRL_SET_TLSEXT_HOSTNAME */
#define OSSL_TLSEXT_NAME_HOST       0    /* TLSEXT_NAMETYPE_host_name */
#define OSSL_CTRL_SET_MIN_PROTO     123  /* SSL_CTRL_SET_MIN_PROTO_VERSION */
#define OSSL_TLS1_2_VERSION         0x0303
#define OSSL_ERROR_WANT_READ        2
#define OSSL_ERROR_WANT_WRITE       3
#define OSSL_ERROR_ZERO_RETURN      6
#define OSSL_ERROR_SYSCALL          5
#define OSSL_X509_V_OK              0

static struct {
    const void *(*client_method)(void);
    ossl_ctx *(*ctx_new)(const void *);
    long (*ctx_ctrl)(ossl_ctx *, int, long, void *);
    void (*ctx_set_verify)(ossl_ctx *, int, void *);
    int (*ctx_default_paths)(ossl_ctx *);
    int (*ctx_load_verify)(ossl_ctx *, const char *, const char *);
    ossl_ssl *(*ssl_new)(ossl_ctx *);
    int (*set_fd)(ossl_ssl *, int);
    long (*ssl_ctrl)(ossl_ssl *, int, long, void *);
    int (*set1_host)(ossl_ssl *, const char *);
    int (*handshake)(ossl_ssl *);
    int (*read)(ossl_ssl *, void *, int);
    int (*write)(ossl_ssl *, const void *, int);
    int (*get_error)(const ossl_ssl *, int);
    long (*verify_result)(const ossl_ssl *);
    const char *(*verify_str)(long); /* libcrypto, via libssl deps; optional */
    int (*shutdown)(ossl_ssl *);
    void (*ssl_free)(ossl_ssl *);
    ossl_ctx *ctx; /* shared client context; also the "loaded" flag */
} ossl;

/* CA bundle fallbacks when the library's compiled-in default dir is empty
 * (e.g. a non-native libssl). SSL_CERT_FILE / SSL_CERT_DIR env still win
 * via SSL_CTX_set_default_verify_paths. */
static const char *const ossl_ca_bundles[] = {
    "/etc/ssl/certs/ca-certificates.crt", /* Debian/Ubuntu/Alpine */
    "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora/RHEL */
    "/etc/ssl/ca-bundle.pem",             /* openSUSE */
    "/etc/ssl/cert.pem",                  /* misc */
    NULL,
};

static int ossl_load(void) {
    if (ossl.ctx) return 0;
    void *h = dlopen("libssl.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libssl.so.1.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libssl.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return -1;
    *(void **)&ossl.client_method    = dlsym(h, "TLS_client_method");
    *(void **)&ossl.ctx_new          = dlsym(h, "SSL_CTX_new");
    *(void **)&ossl.ctx_ctrl         = dlsym(h, "SSL_CTX_ctrl");
    *(void **)&ossl.ctx_set_verify   = dlsym(h, "SSL_CTX_set_verify");
    *(void **)&ossl.ctx_default_paths = dlsym(h, "SSL_CTX_set_default_verify_paths");
    *(void **)&ossl.ctx_load_verify  = dlsym(h, "SSL_CTX_load_verify_locations");
    *(void **)&ossl.ssl_new          = dlsym(h, "SSL_new");
    *(void **)&ossl.set_fd           = dlsym(h, "SSL_set_fd");
    *(void **)&ossl.ssl_ctrl         = dlsym(h, "SSL_ctrl");
    *(void **)&ossl.set1_host        = dlsym(h, "SSL_set1_host");
    *(void **)&ossl.handshake        = dlsym(h, "SSL_connect");
    *(void **)&ossl.read             = dlsym(h, "SSL_read");
    *(void **)&ossl.write            = dlsym(h, "SSL_write");
    *(void **)&ossl.get_error        = dlsym(h, "SSL_get_error");
    *(void **)&ossl.verify_result    = dlsym(h, "SSL_get_verify_result");
    /* dlsym searches libssl's own deps, so this finds libcrypto's symbol */
    *(void **)&ossl.verify_str       = dlsym(h, "X509_verify_cert_error_string");
    *(void **)&ossl.shutdown         = dlsym(h, "SSL_shutdown");
    *(void **)&ossl.ssl_free         = dlsym(h, "SSL_free");
    if (!ossl.client_method || !ossl.ctx_new || !ossl.ctx_ctrl ||
        !ossl.ctx_set_verify || !ossl.ctx_default_paths ||
        !ossl.ctx_load_verify || !ossl.ssl_new || !ossl.set_fd ||
        !ossl.ssl_ctrl || !ossl.set1_host || !ossl.handshake ||
        !ossl.read || !ossl.write || !ossl.get_error ||
        !ossl.verify_result || !ossl.shutdown || !ossl.ssl_free)
        return -1;
    ossl_ctx *ctx = ossl.ctx_new(ossl.client_method());
    if (!ctx) return -1;
    ossl.ctx_ctrl(ctx, OSSL_CTRL_SET_MIN_PROTO, OSSL_TLS1_2_VERSION, NULL);
    ossl.ctx_set_verify(ctx, OSSL_VERIFY_PEER, NULL);
    ossl.ctx_default_paths(ctx); /* system dir + SSL_CERT_FILE/SSL_CERT_DIR */
    if (!getenv("SSL_CERT_FILE") && !getenv("SSL_CERT_DIR")) {
        for (const char *const *p = ossl_ca_bundles; *p; p++) {
            if (access(*p, R_OK) == 0) {
                ossl.ctx_load_verify(ctx, *p, NULL);
                break;
            }
        }
    }
    ossl.ctx = ctx; /* set last: it is the "loaded" flag */
    return 0;
}

static bool ossl_want_retry(int e) {
    return e == OSSL_ERROR_WANT_READ || e == OSSL_ERROR_WANT_WRITE;
}

/* Poll for the direction the last SSL_ERROR_WANT_* asked for. */
static void ossl_wait(int fd, int want, int timeout_ms) {
    struct pollfd pf = {fd, want == OSSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN, 0};
    poll(&pf, 1, timeout_ms);
}
#endif /* __linux__ */

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
#elif defined(__linux__)
    if (ossl_load() != 0) {
        snprintf(err, errlen,
                 "TLS unavailable: system libssl not found (install OpenSSL "
                 "1.1+/3.x shared libraries, e.g. Debian libssl3)");
        nstream_close(s);
        return NULL;
    }
    ossl_ssl *ssl = ossl.ssl_new(ossl.ctx);
    if (!ssl) { snprintf(err, errlen, "TLS context failed"); nstream_close(s); return NULL; }
    ossl.set_fd(ssl, fd);
    /* SNI (SSL_set_tlsext_host_name macro) + hostname verification */
    ossl.ssl_ctrl(ssl, OSSL_CTRL_SET_TLSEXT_HOST, OSSL_TLSEXT_NAME_HOST,
                  (void *)(uintptr_t)host);
    ossl.set1_host(ssl, host);
    int64_t deadline = now_ms() + timeout_ms;
    int rc;
    while ((rc = ossl.handshake(ssl)) != 1) {
        int e = ossl.get_error(ssl, rc);
        if (!ossl_want_retry(e)) {
            long vr = ossl.verify_result(ssl);
            if (vr != OSSL_X509_V_OK && ossl.verify_str)
                snprintf(err, errlen, "TLS certificate for %s rejected: %s",
                         host, ossl.verify_str(vr));
            else
                snprintf(err, errlen, "TLS handshake with %s failed (%d)",
                         host, e);
            ossl.ssl_free(ssl);
            nstream_close(s);
            return NULL;
        }
        if (now_ms() > deadline) {
            snprintf(err, errlen, "TLS handshake with %s timed out", host);
            ossl.ssl_free(ssl);
            nstream_close(s);
            return NULL;
        }
        ossl_wait(fd, e, 100);
    }
    s->ssl = ssl;
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
#elif defined(__linux__)
    if (s->ssl) {
        int want = cap > INT_MAX ? INT_MAX : (int)cap;
        int n = ossl.read((ossl_ssl *)s->ssl, buf, want);
        if (n > 0) return n;
        int e = ossl.get_error((ossl_ssl *)s->ssl, n);
        if (ossl_want_retry(e)) return -2;
        if (e == OSSL_ERROR_ZERO_RETURN) return 0;
        if (e == OSSL_ERROR_SYSCALL) return n == 0 ? 0 : -1; /* 0: EOF, no alert */
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
#elif defined(__linux__)
    if (s->ssl) {
        size_t off = 0;
        while (off < len) {
            size_t left = len - off;
            int want = left > INT_MAX ? INT_MAX : (int)left;
            int n = ossl.write((ossl_ssl *)s->ssl, (const char *)data + off, want);
            if (n > 0) { off += (size_t)n; continue; }
            int e = ossl.get_error((ossl_ssl *)s->ssl, n);
            if (!ossl_want_retry(e)) return -1;
            ossl_wait(s->fd, e, 5000);
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
#elif defined(__linux__)
    if (s->ssl) {
        ossl.shutdown((ossl_ssl *)s->ssl); /* best-effort close_notify */
        ossl.ssl_free((ossl_ssl *)s->ssl);
    }
#endif
    if (s->fd >= 0) close(s->fd);
    free(s);
}
