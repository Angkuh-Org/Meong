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

int utf8_cols(const char *s) {
    size_t n = strlen(s);
    size_t i = 0;
    int cols = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            cols++;
            i++;
            continue;
        }
        wchar_t wc;
        int k = utf8_next(s + i, n - i, &wc);
        if (k <= 0) {
            break;
        }
        int w = wcwidth(wc);
        cols += w < 0 ? 1 : w;
        i += (size_t)k;
    }
    return cols;
}

size_t utf8_tail(const char *s, int cols) {
    size_t n = strlen(s);
    if (cols <= 0) {
        return n;
    }
    int drop = utf8_cols(s) - cols;
    if (drop <= 0) {
        return 0;
    }
    size_t i = 0;
    int dropped = 0;
    while (i < n && dropped < drop) {
        wchar_t wc;
        int k = utf8_next(s + i, n - i, &wc);
        if (k <= 0) {
            break;
        }
        int w = wcwidth(wc);
        dropped += w < 0 ? 1 : w;
        i += (size_t)k;
    }
    return i;
}

static int preview_is_drop(wchar_t wc) {
    return wc < 32 || wc == 0x7f || (wc >= 0x80 && wc <= 0x9f) ||
           (wc >= 0x202a && wc <= 0x202e) ||
           (wc >= 0x2066 && wc <= 0x2069) || wc == 0x200e ||
           wc == 0x200f || wc == 0x061c || (wc >= 0x200b && wc <= 0x200d) ||
           wc == 0xfeff;
}

static size_t pv_ellipsis(char *out, size_t o, size_t cap) {
    if (o + 4 <= cap) {
        memcpy(out + o, "...", 3);
        return o + 3;
    }
    return o;
}

size_t preview_sanitize_line(const char *s, size_t n, char *out, size_t cap,
                             unsigned flags, size_t *dropped) {
    size_t o = 0;
    size_t i = 0;
    size_t dropn = 0;
    if (!out || cap == 0) {
        return 0;
    }
    while (i < n && s[i] != '\n' && s[i] != '\r') {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            dropn++;
            i++;
            if (i < n && s[i] == '[') {
                i++;
                while (i < n && !((unsigned char)s[i] >= 0x40 &&
                                  (unsigned char)s[i] <= 0x7e)) {
                    i++;
                }
                if (i < n) {
                    i++;
                }
            } else if (i < n && s[i] == ']') {
                i++;
                while (i < n) {
                    if (s[i] == 0x07) {
                        i++;
                        break;
                    }
                    if (s[i] == 0x1b && i + 1 < n && s[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
            } else if (i < n) {
                i++;
            }
            continue;
        }
        wchar_t wc = 0;
        int k = utf8_next(s + i, n - i, &wc);
        if (k <= 0) {
            k = 1;
            wc = 0xFFFD;
        }
        if (wc == '\t' && (flags & PV_TABS)) {
            if (o + 5 > cap) {
                o = pv_ellipsis(out, o, cap);
                break;
            }
            memcpy(out + o, "    ", 4);
            o += 4;
            i += (size_t)k;
            continue;
        }
        if (preview_is_drop(wc)) {
            dropn++;
            if (flags & PV_REPLACE) {
                if (o + 2 > cap) {
                    o = pv_ellipsis(out, o, cap);
                    break;
                }
                out[o++] = '?';
            }
            i += (size_t)k;
            continue;
        }
        if (o + (size_t)k + 1 > cap) {
            o = pv_ellipsis(out, o, cap);
            break;
        }
        if (k == 1 && wc == 0xFFFD && c >= 0x80) {
            out[o++] = '?';
        } else {
            memcpy(out + o, s + i, (size_t)k);
            o += (size_t)k;
        }
        i += (size_t)k;
    }
    out[o] = '\0';
    if (dropped) {
        *dropped += dropn;
    }
    return o;
}

/*
 * ASCII fast path. A SWAR 8-byte skip measured 15.9 us vs 55.4 us on 24 KiB
 * ASCII, but was slower on UTF-8 (115 vs 95 us) and this runs once per card,
 * not per token; rejected.
 */
size_t preview_control_count(const char *s, size_t n) {
    size_t count = 0;
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            count++;
            i++;
            if (i < n && s[i] == '[') {
                i++;
                while (i < n && !((unsigned char)s[i] >= 0x40 &&
                                  (unsigned char)s[i] <= 0x7e)) {
                    i++;
                }
                if (i < n) {
                    i++;
                }
            } else if (i < n && s[i] == ']') {
                i++;
                while (i < n) {
                    if (s[i] == 0x07) {
                        i++;
                        break;
                    }
                    if (s[i] == 0x1b && i + 1 < n && s[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
            } else if (i < n) {
                i++;
            }
            continue;
        }
        /*
         * ASCII fast path: \n\r\t are not controls, every other C0 byte and
         * DEL is. This avoids one utf8_next and one preview_is_drop call per
         * byte, which is what a 24 KiB preview used to pay.
         */
        if (c < 0x80) {
            if (c >= 0x20 && c != 0x7f) {
                i++;
                continue;
            }
            if (c != '\n' && c != '\r' && c != '\t') {
                count++;
            }
            i++;
            continue;
        }
        wchar_t wc = 0;
        int k = utf8_next(s + i, n - i, &wc);
        if (k <= 0) {
            count++;
            i++;
            continue;
        }
        if (wc == '\n' || wc == '\r' || wc == '\t') {
            i += (size_t)k;
            continue;
        }
        if (preview_is_drop(wc)) {
            count++;
        }
        i += (size_t)k;
    }
    return count;
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

/* ---- atomic replace ---------------------------------------------------- */

static int write_all_fd(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static void dir_fsync(int dfd) {
    int sfd = openat(dfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (sfd >= 0) {
        (void)fsync(sfd);
        close(sfd);
    }
}

/*
 * Fixed-length, non-embedding temp name. A stale name can never block a
 * retry after a crash because the SipHash key changes with the process; the
 * attempt suffix only handles a same-process collision. Never unlink a name
 * this call did not create.
 */
static void keyed_temp(char *out, size_t cap, const char *base, int attempt) {
    unsigned long long h = meong_siphash24(base, strlen(base));
    snprintf(out, cap, ".mng.%016llx.%d", h, attempt);
}

/*
 * The parent fd comes from the dirfd walk, so a concurrent rename of an
 * intermediate directory cannot redirect the temp or the rename. The temp is
 * hidden, exclusive and no-follow: a planted symlink fails EEXIST, not a
 * truncate. fchown before fchmod because chown clears setuid/setgid.
 *
 * keyed == 0 keeps edit's shipped `.<base>.mng` name and 60 s takeover
 * exactly as before. keyed == 1 is write's non-embedding name and never
 * unlinks an unknown temp.
 */
static int replace_at_impl(int dfd, const char *base, const char *data,
                           size_t len, mode_t mode, uid_t uid, gid_t gid,
                           int keyed) {
    if (dfd < 0 || !base || !*base || strchr(base, '/') ||
        strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t blen = strlen(base);
    if (blen > (keyed ? NAME_MAX : NAME_MAX - 8)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char tmp[NAME_MAX + 8];
    int fd = -1;
    /*
     * keyed == 0 is the legacy `.<base>.mng` path with the 60 s takeover;
     * no tool calls it now (edit and write both use keyed == 1). It remains
     * for write_file_replace()/write_replace_test. The takeover can unlink a
     * user file named `.<base>.mng`; do not route a tool through it.
     */
    int attempts = keyed ? 8 : 1;
    for (int attempt = 0; attempt < attempts; attempt++) {
        if (keyed) {
            keyed_temp(tmp, sizeof tmp, base, attempt);
        } else {
            snprintf(tmp, sizeof tmp, ".%s.mng", base);
        }
        fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                       O_NOFOLLOW, 0600);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            return -1;
        }
        if (keyed) {
            continue;
        }
        struct stat tst;
        if (fstatat(dfd, tmp, &tst, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(tst.st_mode) && tst.st_uid == geteuid() &&
            time(NULL) - tst.st_mtime > 60) {
            unlinkat(dfd, tmp, 0);
            fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                       O_NOFOLLOW, 0600);
            if (fd >= 0) {
                break;
            }
        }
        return -1;
    }
    if (fd < 0) {
        errno = EEXIST;
        return -1;
    }
    if (geteuid() == 0 && (uid != 0 || gid != 0)) {
        if (fchown(fd, uid, gid) != 0) {
            goto fail;
        }
    }
    if (fchmod(fd, mode & 07777) != 0) {
        goto fail;
    }
    if (write_all_fd(fd, data, len) != 0 || fsync(fd) != 0) {
        goto fail;
    }
    if (close(fd) != 0) {
        fd = -1;
        goto fail;
    }
    fd = -1;
    if (renameat(dfd, tmp, dfd, base) != 0) {
        goto fail;
    }
    dir_fsync(dfd);
    return 0;

fail:
    {
        int saved = errno;
        if (fd >= 0) {
            close(fd);
        }
        unlinkat(dfd, tmp, 0);
        errno = saved;
        return -1;
    }
}

int write_file_replace_at(int dfd, const char *base, const char *data,
                          size_t len, mode_t mode, uid_t uid, gid_t gid) {
    return replace_at_impl(dfd, base, data, len, mode, uid, gid, 0);
}

int write_file_replace_at_keyed(int dfd, const char *base, const char *data,
                                size_t len, mode_t mode, uid_t uid,
                                gid_t gid) {
    return replace_at_impl(dfd, base, data, len, mode, uid, gid, 1);
}

/*
 * Create-only publish. The temp is written and fsynced first; the final name
 * appears only through RENAME_NOREPLACE (or linkat when the kernel/filesystem
 * lacks the flag). A plain renameat here would replace a winner, so it is
 * never called. EEXIST means "the name exists", not a retry hint.
 */
int write_file_create_at(int dfd, const char *base, const char *data,
                         size_t len, mode_t mode) {
    if (dfd < 0 || !base || !*base || strchr(base, '/') ||
        strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        errno = EINVAL;
        return -1;
    }
    if (strlen(base) > NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char tmp[NAME_MAX + 8];
    int fd = -1;
    for (int attempt = 0; attempt < 8; attempt++) {
        keyed_temp(tmp, sizeof tmp, base, attempt);
        fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                       O_NOFOLLOW, 0600);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    if (fd < 0) {
        errno = EEXIST;
        return -1;
    }
    if (write_all_fd(fd, data, len) != 0 || fchmod(fd, mode & 07777) != 0 ||
        fsync(fd) != 0) {
        goto fail;
    }
    if (close(fd) != 0) {
        fd = -1;
        goto fail;
    }
    fd = -1;
    if (meong_renameat2_noreplace(dfd, tmp, base) != 0) {
        int e = errno;
        if (e != ENOSYS && e != EINVAL && e != EOPNOTSUPP) {
            errno = e;
            goto fail;
        }
        /*
         * TODO(fix): a crash between this linkat and the unlinkat below
         * leaves target+temp as hardlinks (nlink=2). The target is complete,
         * but edit/write refuse nlink>1 until the temp is removed. renameat2
         * is the normal path; consider a repair rule or drop the fallback on
         * filesystems that cannot publish atomically.
         */
        if (linkat(dfd, tmp, dfd, base, 0) != 0) {
            goto fail;
        }
        (void)unlinkat(dfd, tmp, 0);
    }
    dir_fsync(dfd);
    return 0;

fail:
    {
        int saved = errno;
        if (fd >= 0) {
            close(fd);
        }
        unlinkat(dfd, tmp, 0);
        errno = saved;
        return -1;
    }
}

int write_file_replace(const char *path, const char *data, size_t len) {
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    char dir[PATH_MAX];
    const char *base = strrchr(path, '/');
    if (!base || !base[1]) {
        errno = EINVAL;
        return -1;
    }
    base++;
    size_t dlen = (size_t)(base - path);
    if (dlen == 0) {
        dir[0] = '/';
        dir[1] = '\0';
    } else {
        if (dlen >= sizeof dir) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
        size_t n = strlen(dir);
        while (n > 1 && dir[n - 1] == '/') {
            dir[--n] = '\0';
        }
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int syncable = 1;
    if (dfd < 0 && errno == EACCES) {
        dfd = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        syncable = 0;
    }
    (void)syncable;
    if (dfd < 0) {
        return -1;
    }
    struct stat st;
    if (fstatat(dfd, base, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        int saved = errno;
        close(dfd);
        errno = saved;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        close(dfd);
        errno = EINVAL;
        return -1;
    }
    if (st.st_nlink > 1) {
        close(dfd);
        errno = EMLINK;
        return -1;
    }
    if (st.st_uid != geteuid() && geteuid() != 0) {
        close(dfd);
        errno = EPERM;
        return -1;
    }
    int r = write_file_replace_at(dfd, base, data, len, st.st_mode, st.st_uid,
                                  st.st_gid);
    int saved = errno;
    close(dfd);
    errno = saved;
    return r;
}

/* ---- keyed SipHash-2-4 ------------------------------------------------- */

static unsigned char sip_key[16];
static int sip_key_ready;

static unsigned long long rotl64(unsigned long long x, int b) {
    return (x << b) | (x >> (64 - b));
}

static void sip_round(unsigned long long v[4]) {
    v[0] += v[1];
    v[1] = rotl64(v[1], 13);
    v[1] ^= v[0];
    v[0] = rotl64(v[0], 32);
    v[2] += v[3];
    v[3] = rotl64(v[3], 16);
    v[3] ^= v[2];
    v[0] += v[3];
    v[3] = rotl64(v[3], 21);
    v[3] ^= v[0];
    v[2] += v[1];
    v[1] = rotl64(v[1], 17);
    v[1] ^= v[2];
    v[2] = rotl64(v[2], 32);
}

static void sip_key_init(void) {
    if (sip_key_ready) {
        return;
    }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    int got = 0;
    if (fd >= 0) {
        got = (int)read(fd, sip_key, sizeof sip_key);
        close(fd);
    }
    if (got != (int)sizeof sip_key) {
        /* A weak key is still better than a fixed one; this path is only for
         * a host with no /dev/urandom. The control is stale-write detection,
         * not a cryptographic boundary. */
        unsigned long long seed = (unsigned long long)time(NULL) ^
                                  ((unsigned long long)getpid() << 32);
        for (size_t i = 0; i < sizeof sip_key; i++) {
            seed = seed * 6364136223846793005ULL + 1;
            sip_key[i] = (unsigned char)(seed >> 56);
        }
    }
    sip_key_ready = 1;
}

unsigned long long meong_siphash24(const void *data, size_t len) {
    sip_key_init();
    unsigned long long k0, k1;
    memcpy(&k0, sip_key, 8);
    memcpy(&k1, sip_key + 8, 8);
    unsigned long long v[4] = {
        0x736f6d6570736575ULL ^ k0,
        0x646f72616e646f6dULL ^ k1,
        0x6c7967656e657261ULL ^ k0,
        0x7465646279746573ULL ^ k1,
    };
    const unsigned char *p = data;
    size_t n = len;
    for (; n >= 8; n -= 8, p += 8) {
        unsigned long long m;
        memcpy(&m, p, 8);
        v[3] ^= m;
        sip_round(v);
        sip_round(v);
        v[0] ^= m;
    }
    unsigned long long b = ((unsigned long long)n) << 56;
    switch (n) {
    case 7:
        b |= (unsigned long long)p[6] << 48;
        /* fall through */
    case 6:
        b |= (unsigned long long)p[5] << 40;
        /* fall through */
    case 5:
        b |= (unsigned long long)p[4] << 32;
        /* fall through */
    case 4:
        b |= (unsigned long long)p[3] << 24;
        /* fall through */
    case 3:
        b |= (unsigned long long)p[2] << 16;
        /* fall through */
    case 2:
        b |= (unsigned long long)p[1] << 8;
        /* fall through */
    case 1:
        b |= (unsigned long long)p[0];
        break;
    default:
        break;
    }
    v[3] ^= b;
    sip_round(v);
    sip_round(v);
    v[0] ^= b;
    v[2] ^= 0xff;
    sip_round(v);
    sip_round(v);
    sip_round(v);
    sip_round(v);
    return v[0] ^ v[1] ^ v[2] ^ v[3];
}
