#include "net.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_PORT 8899
#define HEADER_MAX (128u << 10)
#define LINE_MAX (8u << 20)
#define FALLBACK_MAX (1u << 20)

enum {
    ST_SEND,
    ST_HEADERS,
    ST_BODY,
    ST_DONE,
    ST_ERROR,
};

enum {
    CH_SIZE,
    CH_DATA,
    CH_DATA_CRLF,
    CH_TRAILER,
};

struct Stream {
    int fd;
    int state;
    int status;
    int expect_sse;
    int is_sse;
    int chunked;
    long long content_rem;
    int chunk_state;
    char chunk_line[160];
    size_t chunk_line_len;
    long long chunk_rem;
    Buf req;
    size_t sent;
    Buf headers;
    Buf body;
    Buf line;
    Buf fallback;
    int saw_data_field;
    int saw_event;
    sse_cb cb;
    void *ud;
    double t_start;
    double t_last;
    int idle_timeout_ms;
    char content_type[160];
    char err[512];
    Endpoint ep;
};

static void set_error(Stream *s, const char *fmt, ...) {
    if (s->state == ST_ERROR) {
        return;
    }
    s->state = ST_ERROR;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err, sizeof s->err, fmt, ap);
    va_end(ap);
    if (getenv("MEONG_WIRE_DEBUG")) {
        fprintf(stderr, "wire: error: %s\n", s->err);
    }
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

static int wire_debug(void) {
    static int v = -1;
    if (v < 0) {
        v = getenv("MEONG_WIRE_DEBUG") != NULL;
    }
    return v;
}

static void set_done(Stream *s) {
    if (s->state == ST_DONE || s->state == ST_ERROR) {
        return;
    }
    s->state = ST_DONE;
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

static int parse_port(const char *s, int *out) {
    if (!s || !*s) {
        return -1;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v < 1 || v > 65535) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

int endpoint_parse(const char *url, Endpoint *ep, char *err, size_t errlen) {
    memset(ep, 0, sizeof *ep);
    if (!url || !*url) {
        url = "http://127.0.0.1:8899";
    }
    char tmp[1024];
    if (strlen(url) >= sizeof tmp) {
        snprintf(err, errlen, "url is too long");
        return -1;
    }
    strcpy(tmp, url);
    char *p = tmp;
    if (strncasecmp(p, "https://", 8) == 0) {
        snprintf(err, errlen,
                 "https is not supported: Meong speaks plain HTTP to PAIG");
        return -1;
    }
    if (strncasecmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strstr(p, "://")) {
        snprintf(err, errlen, "unsupported url scheme");
        return -1;
    }
    char *path = strchr(p, '/');
    if (path) {
        *path++ = '\0';
    }
    char host[256];
    int port = DEFAULT_PORT;
    if (*p == '[') {
        char *close = strchr(p, ']');
        if (!close) {
            snprintf(err, errlen, "bad IPv6 host in url");
            return -1;
        }
        *close = '\0';
        snprintf(host, sizeof host, "%s", p + 1);
        char *rest = close + 1;
        if (*rest == ':') {
            if (parse_port(rest + 1, &port) != 0) {
                snprintf(err, errlen, "bad port in url");
                return -1;
            }
        } else if (*rest) {
            snprintf(err, errlen, "bad url authority");
            return -1;
        }
        snprintf(ep->host_header, sizeof ep->host_header, "[%s]:%d", host, port);
    } else {
        char *colon = strrchr(p, ':');
        if (colon && strchr(p, ':') == colon) {
            *colon = '\0';
            if (parse_port(colon + 1, &port) != 0) {
                snprintf(err, errlen, "bad port in url");
                return -1;
            }
        } else if (colon) {
            snprintf(err, errlen, "IPv6 hosts must be written as [addr]:port");
            return -1;
        }
        if (!*p) {
            snprintf(err, errlen, "empty host in url");
            return -1;
        }
        size_t hl = strlen(p);
        if (hl >= sizeof host) {
            snprintf(err, errlen, "host is too long");
            return -1;
        }
        memcpy(host, p, hl + 1);
        snprintf(ep->host_header, sizeof ep->host_header, "%s:%d", host, port);
    }
    if (!*host) {
        snprintf(err, errlen, "empty host in url");
        return -1;
    }
    memcpy(ep->host, host, strlen(host) + 1);
    ep->port = port;

    ep->base[0] = '\0';
    if (path && *path) {
        snprintf(ep->base, sizeof ep->base, "/%s", path);
        size_t n = strlen(ep->base);
        while (n > 1 && ep->base[n - 1] == '/') {
            ep->base[--n] = '\0';
        }
        if (strcmp(ep->base, "/v1") == 0) {
            ep->base[0] = '\0';
        } else if (n > 3 && strcmp(ep->base + n - 3, "/v1") == 0) {
            ep->base[n - 3] = '\0';
        }
        if (strcmp(ep->base, "/") == 0) {
            ep->base[0] = '\0';
        }
    }
    return 0;
}

static int open_connection(const Endpoint *ep, char *err, size_t errlen) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", ep->port);
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(ep->host, portstr, &hints, &res);
    if (gai != 0) {
        snprintf(err, errlen, "resolve %s: %s", ep->host, gai_strerror(gai));
        return -1;
    }
    int saved = ECONNREFUSED;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family,
                        ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        ai->ai_protocol);
        if (fd < 0) {
            saved = errno;
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r == 0) {
            freeaddrinfo(res);
            return fd;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 700);
            if (pr > 0) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 &&
                    soerr == 0) {
                    freeaddrinfo(res);
                    return fd;
                }
                saved = soerr ? soerr : ECONNREFUSED;
            } else if (pr == 0) {
                saved = ETIMEDOUT;
            } else {
                saved = errno;
            }
        } else {
            saved = errno;
        }
        close(fd);
    }
    freeaddrinfo(res);
    snprintf(err, errlen, "connect %s:%d: %s", ep->host, ep->port,
             strerror(saved));
    return -1;
}

static void build_request(Stream *s, const Endpoint *ep, const char *method,
                          const char *path, const char *body,
                          size_t body_len) {
    char fullpath[512];
    if (path[0] == '/') {
        snprintf(fullpath, sizeof fullpath, "%s%s", ep->base, path);
    } else {
        snprintf(fullpath, sizeof fullpath, "%s/%s", ep->base, path);
    }
    buf_printf(&s->req, "%s %s HTTP/1.1\r\n", method, fullpath);
    buf_printf(&s->req, "Host: %s\r\n", ep->host_header);
    buf_printf(&s->req, "User-Agent: Meong/0.1\r\n");
    buf_printf(&s->req, "Accept: %s\r\n",
               s->expect_sse ? "text/event-stream" : "application/json");
    buf_printf(&s->req, "Accept-Encoding: identity\r\n");
    buf_printf(&s->req, "Cache-Control: no-cache\r\n");
    buf_printf(&s->req, "Connection: close\r\n");
    if (body) {
        buf_printf(&s->req, "Content-Type: application/json\r\n");
        buf_printf(&s->req, "Content-Length: %zu\r\n", body_len);
    }
    buf_printf(&s->req, "\r\n");
    if (body && body_len) {
        buf_add(&s->req, body, body_len);
    }
}

int stream_start(Stream **out, const Endpoint *ep, const char *method,
                 const char *path, const char *body, size_t body_len,
                 int expect_sse, sse_cb cb, void *ud, int idle_timeout_ms,
                 char *err, size_t errlen) {
    *out = NULL;
    Stream *s = xmalloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->fd = -1;
    s->status = 0;
    s->expect_sse = expect_sse;
    s->content_rem = -1;
    s->chunk_state = CH_SIZE;
    s->cb = cb;
    s->ud = ud;
    s->idle_timeout_ms = idle_timeout_ms > 0 ? idle_timeout_ms : 180000;
    s->ep = *ep;
    buf_init(&s->req);
    buf_init(&s->headers);
    buf_init(&s->body);
    buf_init(&s->line);
    buf_init(&s->fallback);

    s->fd = open_connection(ep, err, errlen);
    if (s->fd < 0) {
        stream_free(s);
        return -1;
    }
    build_request(s, ep, method, path, body, body_len);
    s->state = ST_SEND;
    s->t_start = now_sec();
    s->t_last = s->t_start;
    *out = s;
    return 0;
}

static void process_sse_line(Stream *s);

static void body_complete(Stream *s) {
    if (s->state == ST_DONE || s->state == ST_ERROR) {
        return;
    }
    if (s->is_sse) {
        if (s->line.len > 0) {
            process_sse_line(s);
        }
        if (s->state == ST_DONE || s->state == ST_ERROR) {
            return;
        }
        if (!s->saw_data_field && s->fallback.len > 0) {
            size_t n = s->fallback.len;
            char *p = s->fallback.p;
            while (n > 0 && isspace((unsigned char)p[n - 1])) {
                n--;
            }
            size_t off = 0;
            while (off < n && isspace((unsigned char)p[off])) {
                off++;
            }
            if (n > off) {
                s->saw_event = 1;
                if (s->cb) {
                    s->cb(s->ud, p + off, n - off);
                }
            }
        }
    }
    set_done(s);
}

static void add_fallback(Stream *s, const char *p, size_t n) {
    if (s->saw_data_field) {
        return;
    }
    if (s->fallback.len + n + 2 > FALLBACK_MAX) {
        set_error(s, "response line is too large");
        return;
    }
    buf_add(&s->fallback, p, n);
    buf_addc(&s->fallback, '\n');
}

static void process_sse_line(Stream *s) {
    char *p = s->line.p;
    size_t n = s->line.len;
    if (!p) {
        return;
    }
    while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == '\n')) {
        n--;
    }
    if (wire_debug()) {
        fprintf(stderr, "wire: line[%zu] %.*s\n", n, (int)(n > 120 ? 120 : n),
                p);
    }
    if (n == 0) {
        s->line.len = 0;
        p[0] = '\0';
        return;
    }
    if (n >= 5 && memcmp(p, "data:", 5) == 0) {
        s->saw_data_field = 1;
        char *d = p + 5;
        size_t dn = n - 5;
        if (dn > 0 && *d == ' ') {
            d++;
            dn--;
        }
        while (dn > 0 && isspace((unsigned char)*d)) {
            d++;
            dn--;
        }
        while (dn > 0 && isspace((unsigned char)d[dn - 1])) {
            dn--;
        }
        if (dn == 5 && memcmp(d, "[DONE]", 5) == 0) {
            s->saw_event = 1;
            set_done(s);
        } else if (dn > 0) {
            s->saw_event = 1;
            if (s->cb) {
                s->cb(s->ud, d, dn);
            }
        }
    } else if (p[0] == ':') {
        /* SSE comment */
    } else if ((n >= 6 && memcmp(p, "event:", 6) == 0) ||
               (n >= 3 && memcmp(p, "id:", 3) == 0) ||
               (n >= 6 && memcmp(p, "retry:", 6) == 0)) {
        /* ignored fields */
    } else {
        add_fallback(s, p, n);
    }
    s->line.len = 0;
    p[0] = '\0';
}

static void sse_bytes(Stream *s, const char *p, size_t n) {
    size_t off = 0;
    while (off < n && s->state != ST_DONE && s->state != ST_ERROR) {
        const char *nl = memchr(p + off, '\n', n - off);
        size_t run = nl ? (size_t)(nl - (p + off)) : n - off;
        if (run > 0) {
            if (s->line.len + run > LINE_MAX) {
                set_error(s, "response line is too large");
                return;
            }
            buf_add(&s->line, p + off, run);
            off += run;
        }
        if (!nl) {
            return;
        }
        process_sse_line(s);
        off++;
    }
}

static void emit_body(Stream *s, const char *p, size_t n) {
    if (n == 0) {
        return;
    }
    if (s->is_sse) {
        sse_bytes(s, p, n);
    } else {
        buf_add(&s->body, p, n);
    }
}

static void chunk_feed(Stream *s, const char *p, size_t n) {
    while (n > 0 && s->state != ST_DONE && s->state != ST_ERROR) {
        switch (s->chunk_state) {
        case CH_SIZE: {
            char c = *p++;
            n--;
            if (c == '\n') {
                while (s->chunk_line_len > 0 &&
                       s->chunk_line[s->chunk_line_len - 1] == '\r') {
                    s->chunk_line_len--;
                }
                s->chunk_line[s->chunk_line_len] = '\0';
                char *end = NULL;
                long long sz = strtoll(s->chunk_line, &end, 16);
                if (s->chunk_line_len == 0 || !end || sz < 0) {
                    set_error(s, "bad chunk size");
                    return;
                }
                if (wire_debug()) {
                    fprintf(stderr, "wire: chunk %lld (%s)\n", sz,
                            s->chunk_line);
                }
                s->chunk_line_len = 0;
                if (sz == 0) {
                    s->chunk_state = CH_TRAILER;
                } else {
                    s->chunk_rem = sz;
                    s->chunk_state = CH_DATA;
                }
            } else if (s->chunk_line_len + 1 < sizeof s->chunk_line) {
                s->chunk_line[s->chunk_line_len++] = c;
            } else {
                set_error(s, "chunk size line is too long");
                return;
            }
            break;
        }
        case CH_DATA: {
            size_t take = n;
            if ((long long)take > s->chunk_rem) {
                take = (size_t)s->chunk_rem;
            }
            emit_body(s, p, take);
            p += take;
            n -= take;
            s->chunk_rem -= (long long)take;
            if (s->chunk_rem == 0) {
                s->chunk_state = CH_DATA_CRLF;
            }
            break;
        }
        case CH_DATA_CRLF:
            while (n > 0 && *p != '\n') {
                p++;
                n--;
            }
            if (n > 0 && *p == '\n') {
                p++;
                n--;
                s->chunk_state = CH_SIZE;
            }
            break;
        case CH_TRAILER: {
            char c = *p++;
            n--;
            if (c == '\n') {
                while (s->chunk_line_len > 0 &&
                       s->chunk_line[s->chunk_line_len - 1] == '\r') {
                    s->chunk_line_len--;
                }
                if (s->chunk_line_len == 0) {
                    body_complete(s);
                    return;
                }
                s->chunk_line_len = 0;
            } else if (s->chunk_line_len + 1 < sizeof s->chunk_line) {
                s->chunk_line[s->chunk_line_len++] = c;
            }
            break;
        }
        }
    }
}

static void feed(Stream *s, const char *p, size_t n) {
    if (s->state != ST_BODY || n == 0) {
        return;
    }
    if (s->chunked) {
        chunk_feed(s, p, n);
        return;
    }
    if (s->content_rem >= 0) {
        size_t take = n;
        if ((long long)take > s->content_rem) {
            take = (size_t)s->content_rem;
        }
        emit_body(s, p, take);
        s->content_rem -= (long long)take;
        if (s->content_rem == 0) {
            body_complete(s);
        }
        return;
    }
    emit_body(s, p, n);
}

static size_t find_header_end(const char *p, size_t n) {
    if (n < 4) {
        return (size_t)-1;
    }
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' &&
            p[i + 3] == '\n') {
            return i;
        }
    }
    return (size_t)-1;
}

static int parse_headers(Stream *s) {
    size_t endpos = find_header_end(s->headers.p, s->headers.len);
    if (endpos == (size_t)-1) {
        set_error(s, "response headers are incomplete");
        return -1;
    }
    size_t body_off = endpos + 4;
    s->headers.p[endpos] = '\0';
    char *line_end = strstr(s->headers.p, "\r\n");
    if (!line_end) {
        set_error(s, "malformed response status line");
        return -1;
    }
    *line_end = '\0';
    if (sscanf(s->headers.p, "HTTP/%*d.%*d %d", &s->status) != 1) {
        set_error(s, "malformed response status line");
        return -1;
    }
    s->chunked = 0;
    s->content_rem = -1;
    s->content_type[0] = '\0';
    char *save = NULL;
    for (char *line = strtok_r(line_end + 2, "\r\n", &save); line;
         line = strtok_r(NULL, "\r\n", &save)) {
        char *colon = strchr(line, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') {
            val++;
        }
        size_t vn = strlen(val);
        while (vn > 0 && (val[vn - 1] == ' ' || val[vn - 1] == '\t')) {
            val[--vn] = '\0';
        }
        if (strcasecmp(line, "transfer-encoding") == 0 &&
            str_contains_ci(val, "chunked")) {
            s->chunked = 1;
        } else if (strcasecmp(line, "content-length") == 0) {
            char *end = NULL;
            long long v = strtoll(val, &end, 10);
            if (end && *end == '\0' && v >= 0) {
                s->content_rem = v;
            }
        } else if (strcasecmp(line, "content-type") == 0) {
            snprintf(s->content_type, sizeof s->content_type, "%s", val);
        }
    }
    s->is_sse = s->expect_sse && s->status == 200 &&
                str_contains_ci(s->content_type, "text/event-stream");
    if (s->status != 200) {
        s->is_sse = 0;
    }
    if (wire_debug()) {
        fprintf(stderr,
                "wire: status=%d type=%s chunked=%d len=%lld sse=%d hdr=%zu\n",
                s->status, s->content_type, s->chunked, s->content_rem,
                s->is_sse, s->headers.len);
    }
    s->state = ST_BODY;
    if (body_off < s->headers.len) {
        feed(s, s->headers.p + body_off, s->headers.len - body_off);
    }
    buf_free(&s->headers);
    if (s->state == ST_BODY && !s->chunked && s->content_rem == 0) {
        body_complete(s);
    }
    return 0;
}

static void finish_eof(Stream *s) {
    if (s->chunked) {
        set_error(s, "stream ended before the final chunk");
        return;
    }
    if (s->content_rem > 0) {
        set_error(s, "connection closed with %lld body bytes missing",
                  s->content_rem);
        return;
    }
    if (s->is_sse) {
        if (s->line.len > 0) {
            process_sse_line(s);
        }
        if (!s->saw_data_field && s->fallback.len > 0) {
            size_t n = s->fallback.len;
            char *p = s->fallback.p;
            while (n > 0 && isspace((unsigned char)p[n - 1])) {
                n--;
            }
            if (n > 0) {
                s->saw_event = 1;
                if (s->cb) {
                    s->cb(s->ud, p, n);
                }
            }
        }
    }
    set_done(s);
}

int stream_tick(Stream *s, size_t *progress) {
    if (!s) {
        return -1;
    }
    if (s->state == ST_DONE) {
        return 1;
    }
    if (s->state == ST_ERROR) {
        return -1;
    }
    double now = now_sec();
    double limit = (double)s->idle_timeout_ms / 1000.0;
    if (now - s->t_last > limit) {
        set_error(s, "gateway timed out after %.0fs of silence", limit);
        return -1;
    }
    for (;;) {
        if (s->state == ST_SEND) {
            while (s->sent < s->req.len) {
                ssize_t n = send(s->fd, s->req.p + s->sent,
                                 s->req.len - s->sent, MSG_NOSIGNAL);
                if (n > 0) {
                    s->sent += (size_t)n;
                    s->t_last = now_sec();
                    if (progress) {
                        *progress += (size_t)n;
                    }
                } else if (n < 0 &&
                           (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return 0;
                } else if (n < 0 && errno == EINTR) {
                    continue;
                } else {
                    set_error(s, "send: %s", strerror(errno));
                    return -1;
                }
            }
            s->state = ST_HEADERS;
            continue;
        }
        if (s->state == ST_HEADERS) {
            char tmp[16384];
            for (;;) {
                ssize_t n = recv(s->fd, tmp, sizeof tmp, 0);
                if (n > 0) {
                    if (s->headers.len + (size_t)n > HEADER_MAX) {
                        set_error(s, "response headers are too large");
                        return -1;
                    }
                    buf_add(&s->headers, tmp, (size_t)n);
                    s->t_last = now_sec();
                    if (progress) {
                        *progress += (size_t)n;
                    }
                } else if (n == 0) {
                    set_error(s, "connection closed before response headers");
                    return -1;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return 0;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    set_error(s, "recv: %s", strerror(errno));
                    return -1;
                }
                if (find_header_end(s->headers.p, s->headers.len) !=
                    (size_t)-1) {
                    break;
                }
            }
            if (parse_headers(s) != 0) {
                return -1;
            }
            if (s->state == ST_DONE) {
                return 1;
            }
            if (s->state == ST_ERROR) {
                return -1;
            }
            continue;
        }
        if (s->state == ST_BODY) {
            char tmp[32768];
            for (;;) {
                ssize_t n = recv(s->fd, tmp, sizeof tmp, 0);
                if (n > 0) {
                    s->t_last = now_sec();
                    if (progress) {
                        *progress += (size_t)n;
                    }
                    feed(s, tmp, (size_t)n);
                    if (s->state == ST_DONE) {
                        return 1;
                    }
                    if (s->state == ST_ERROR) {
                        return -1;
                    }
                } else if (n == 0) {
                    finish_eof(s);
                    return s->state == ST_DONE ? 1 : -1;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return 0;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    set_error(s, "recv: %s", strerror(errno));
                    return -1;
                }
            }
        }
        return s->state == ST_DONE ? 1 : 0;
    }
}

void stream_free(Stream *s) {
    if (!s) {
        return;
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    buf_free(&s->req);
    buf_free(&s->headers);
    buf_free(&s->body);
    buf_free(&s->line);
    buf_free(&s->fallback);
    free(s);
}

int stream_status(const Stream *s) {
    return s ? s->status : 0;
}

int stream_was_sse(const Stream *s) {
    return s ? s->is_sse : 0;
}

const Buf *stream_body(const Stream *s) {
    return s ? &s->body : NULL;
}

const char *stream_error(const Stream *s) {
    return s ? s->err : "";
}

const char *stream_content_type(const Stream *s) {
    return s ? s->content_type : "";
}
