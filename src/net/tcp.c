#include "net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

int url_parse(const char *url, url_parts *out) {
    memset(out, 0, sizeof *out);
    const char *p = strstr(url, "://");
    if (!p || (size_t)(p - url) >= sizeof out->scheme) return -1;
    memcpy(out->scheme, url, (size_t)(p - url));
    p += 3;
    if (strcmp(out->scheme, "unix") == 0) {
        snprintf(out->path, sizeof out->path, "%s", p);
        return 0;
    }
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));
    if (colon) {
        size_t hl = (size_t)(colon - p);
        if (hl >= sizeof out->host) return -1;
        memcpy(out->host, p, hl);
        out->port = atoi(colon + 1);
    } else {
        size_t hl = (size_t)(hostend - p);
        if (hl >= sizeof out->host) return -1;
        memcpy(out->host, p, hl);
        if (strcmp(out->scheme, "https") == 0 || strcmp(out->scheme, "wss") == 0)
            out->port = 443;
        else out->port = 80;
    }
    snprintf(out->path, sizeof out->path, "%s", slash ? slash : "/");
    if (!out->host[0] || out->port <= 0) return -1;
    return 0;
}

int set_nonblock(int fd, bool nb) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

int tcp_connect(const char *host, int port, int timeout_ms) {
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        set_nonblock(fd, true);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) break;
        if (errno == EINPROGRESS) {
            struct pollfd pf = {fd, POLLOUT, 0};
            if (poll(&pf, 1, timeout_ms) == 1) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr == 0) break;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

int unix_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) { close(fd); return -1; }
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    set_nonblock(fd, true);
    return fd;
}
