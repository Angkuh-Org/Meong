#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("Meong: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("Meong: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

void buf_init(Buf *b) {
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_clear(Buf *b) {
    b->len = 0;
    if (b->p) {
        b->p[0] = '\0';
    }
}

void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) {
        return;
    }
    size_t need = b->len + extra + 1;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        cap += cap / 2;
    }
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

void buf_add(Buf *b, const void *data, size_t len) {
    buf_reserve(b, len);
    memcpy(b->p + b->len, data, len);
    b->len += len;
    b->p[b->len] = '\0';
}

void buf_adds(Buf *b, const char *s) {
    buf_add(b, s, strlen(s));
}

void buf_addc(Buf *b, char c) {
    buf_reserve(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = '\0';
}

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void buf_json_str(Buf *b, const char *s, size_t len) {
    static const char hex[] = "0123456789abcdef";
    buf_addc(b, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': buf_adds(b, "\\\""); break;
        case '\\': buf_adds(b, "\\\\"); break;
        case '\b': buf_adds(b, "\\b"); break;
        case '\f': buf_adds(b, "\\f"); break;
        case '\n': buf_adds(b, "\\n"); break;
        case '\r': buf_adds(b, "\\r"); break;
        case '\t': buf_adds(b, "\\t"); break;
        default:
            if (c < 0x20) {
                char tmp[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0};
                buf_adds(b, tmp);
            } else {
                buf_addc(b, (char)c);
            }
        }
    }
    buf_addc(b, '"');
}

char *buf_take(Buf *b) {
    char *p = b->p;
    if (!p) {
        p = xstrdup("");
    }
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
    return p;
}

double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int utf8_valid_cont(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

int utf8_next(const char *s, size_t n, wchar_t *out) {
    const unsigned char *p = (const unsigned char *)s;
    if (n == 0) {
        return 0;
    }
    unsigned char c = p[0];
    if (c < 0x80) {
        *out = (wchar_t)c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF && n >= 2 && utf8_valid_cont(p[1])) {
        *out = (wchar_t)(((c & 0x1F) << 6) | (p[1] & 0x3F));
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF && n >= 3 && utf8_valid_cont(p[1]) &&
        utf8_valid_cont(p[2])) {
        wchar_t wc = (wchar_t)(((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) |
                               (p[2] & 0x3F));
        if (wc < 0x800 || (wc >= 0xD800 && wc <= 0xDFFF)) {
            *out = 0xFFFD;
            return 1;
        }
        *out = wc;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4 && n >= 4 && utf8_valid_cont(p[1]) &&
        utf8_valid_cont(p[2]) && utf8_valid_cont(p[3])) {
        wchar_t wc = (wchar_t)(((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
                               ((p[2] & 0x3F) << 6) | (p[3] & 0x3F));
        if (wc < 0x10000 || wc > 0x10FFFF) {
            *out = 0xFFFD;
            return 1;
        }
        *out = wc;
        return 4;
    }
    *out = 0xFFFD;
    return 1;
}

int utf8_encode(wchar_t wc, char out[4]) {
    unsigned int c = (unsigned int)wc;
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    if (c <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (c >> 18));
        out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[3] = (char)(0x80 | (c & 0x3F));
        return 4;
    }
    out[0] = '?';
    return 1;
}

char *wcs_to_utf8(const wchar_t *s, size_t n) {
    Buf b;
    buf_init(&b);
    mbstate_t st;
    memset(&st, 0, sizeof st);
    for (size_t i = 0; i < n; i++) {
        char tmp[MB_LEN_MAX];
        size_t k = wcrtomb(tmp, s[i], &st);
        if (k == (size_t)-1) {
            memset(&st, 0, sizeof st);
            buf_addc(&b, '?');
        } else {
            buf_add(&b, tmp, k);
        }
    }
    if (!b.p) {
        return xstrdup("");
    }
    return buf_take(&b);
}

int str_contains_ci(const char *hay, const char *needle) {
    if (!*needle) {
        return 1;
    }
    for (const char *h = hay; *h; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) {
            return 1;
        }
    }
    return 0;
}

int str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
    return s;
}

void fmt_count(long long v, char *out, size_t cap) {
    if (v < 1000) {
        snprintf(out, cap, "%lld", v);
    } else if (v < 1000000) {
        double d = (double)v / 1000.0;
        if (d >= 100.0) {
            snprintf(out, cap, "%.0fk", d);
        } else {
            snprintf(out, cap, "%.1fk", d);
        }
    } else {
        double d = (double)v / 1000000.0;
        if (d >= 100.0) {
            snprintf(out, cap, "%.0fM", d);
        } else {
            snprintf(out, cap, "%.1fM", d);
        }
    }
}

char *read_file(const char *path, size_t *len_out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (64 << 20)) {
        close(fd);
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    char *buf = xmalloc(len + 1);
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    buf[got] = '\0';
    if (len_out) {
        *len_out = got;
    }
    return buf;
}

int mkdir_p(const char *path) {
    if (!path || !*path) {
        return 0;
    }
    char tmp[PATH_MAX];
    if (strlen(path) >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(tmp, path);
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') {
        tmp[--n] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int write_file_atomic(const char *path, const char *data, size_t len) {
    char dir[PATH_MAX];
    if (strlen(path) >= sizeof dir) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(dir, path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        if (slash == dir) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
        if (mkdir_p(dir) != 0) {
            return -1;
        }
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)n;
    }
    if (close(fd) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
