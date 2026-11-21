/*
 * write: the second mutating tool. prepare() decides create vs replace from
 * one lstat, builds the preview and a content ticket; run() re-checks the
 * ticket and publishes. It never calls authorize: the layer gated the call.
 */
#include "tools/write.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/util.h"

#define WRITE_MAX_CONTENT (24u << 10)
#define WRITE_MAX_TARGET (1u << 20)
#define WRITE_PREVIEW_LINE 68
#define WRITE_PREVIEW_PATH 96

typedef struct {
    char *path;       /* resolved, canonical */
    char *content;
    size_t content_len;
    size_t old_size;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    int existing;
    int identical;
} WritePlan;

static void plan_free(void *p) {
    WritePlan *plan = p;
    if (!plan) {
        return;
    }
    free(plan->path);
    free(plan->content);
    free(plan);
}

static int failf(char *err, size_t cap, const char *fmt, ...) {
    if (err && cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, cap, fmt, ap);
        va_end(ap);
    }
    return TOOL_ERR;
}

static int write_error(char *err, size_t cap, const char *path, int e) {
    switch (e) {
    case EEXIST:
        return failf(err, cap, "ERROR: write: %s already exists; re-run the "
                               "write", path);
    case EACCES:
    case EPERM:
        return failf(err, cap, "ERROR: write: permission denied: %s", path);
    case ENOENT:
        return failf(err, cap,
                     "ERROR: write: parent directory does not exist: %s",
                     path);
    case ENOTDIR:
        return failf(err, cap, "ERROR: write: not a directory in path: %s",
                     path);
    case ENAMETOOLONG:
        return failf(err, cap, "ERROR: write: file name too long: %s", path);
    case ENOSPC:
        return failf(err, cap, "ERROR: write: no space left on device");
    case EDQUOT:
        return failf(err, cap, "ERROR: write: disk quota exceeded");
    case EIO:
        return failf(err, cap, "ERROR: write: I/O error");
    case EINVAL:
    case EOPNOTSUPP:
        return failf(err, cap, "ERROR: write: %s cannot create atomically",
                     path);
    default:
        return failf(err, cap, "ERROR: write: write failed for %s: %s", path,
                     strerror(e));
    }
}

static mode_t current_umask(void) {
    mode_t m = umask(0);
    (void)umask(m);
    return m;
}

static int read_all(int fd, Buf *b, char *err, size_t cap) {
    char tmp[8192];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return failf(err, cap, "ERROR: write: read failed: %s",
                         strerror(errno));
        }
        if (n == 0) {
            return 0;
        }
        if (b->len + (size_t)n > WRITE_MAX_TARGET) {
            return failf(err, cap,
                         "ERROR: write: file too large: over %u B",
                         (unsigned)WRITE_MAX_TARGET);
        }
        buf_add(b, tmp, (size_t)n);
    }
}

static size_t count_lines(const char *s, size_t n) {
    size_t lines = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            lines++;
        } else if (s[i] == '\r') {
            lines++;
            if (i + 1 < n && s[i + 1] == '\n') {
                i++;
            }
        }
    }
    if (n > 0 && s[n - 1] != '\n' && s[n - 1] != '\r') {
        lines++;
    }
    return lines;
}

static void next_line(const char *s, size_t n, size_t *pos,
                      const char **line, size_t *len) {
    size_t i = *pos;
    size_t start = i;
    while (i < n && s[i] != '\n' && s[i] != '\r') {
        i++;
    }
    *line = s + start;
    *len = i - start;
    if (i < n && s[i] == '\r') {
        i++;
        if (i < n && s[i] == '\n') {
            i++;
        }
    } else if (i < n && s[i] == '\n') {
        i++;
    }
    *pos = i;
}

static void elide_path(const char *in, char *out, size_t cap) {
    size_t n = strlen(in);
    if (n <= WRITE_PREVIEW_PATH) {
        snprintf(out, cap, "%s", in);
        return;
    }
    size_t keep = WRITE_PREVIEW_PATH - 3;
    snprintf(out, cap, "...%s", in + n - keep);
}

/*
 * noinline: the PATH_MAX scratch would otherwise land in tool_prep_write's
 * frame on every call, even though only the !overwrite error path needs it.
 */
static __attribute__((noinline)) void short_result(const char *cwd,
                                                   const char *resolved,
                                                   char *out, size_t cap) {
    char sp[PATH_MAX];
    tool_short_path(resolved, cwd, sp, sizeof sp);
    elide_path(sp, out, cap);
}

static void append_sample(Buf *b, const char *p, size_t n) {
    char line[WRITE_PREVIEW_LINE + 4];
    line[0] = '|';
    line[1] = ' ';
    preview_sanitize_line(p, n, line + 2, WRITE_PREVIEW_LINE,
                          PV_TABS | PV_ELLIPSIS, NULL);
    buf_addc(b, '\n');
    buf_adds(b, line);
}

/*
 * TODO(mem): sp+safe[PATH_MAX] is 8,528 B; read_all's 8 KiB chunk is the other
 * write-prep peak. Shrinking either alone saves <300 B of worst-case stack;
 * revisit both together if this path ever needs a smaller frame.
 */
static void build_preview(const char *cwd, const char *resolved, int create,
                          size_t old_size, const char *content,
                          size_t content_len, mode_t mode, int identical,
                          ToolPrep *out) {
    char sp[PATH_MAX];
    char safe[PATH_MAX];
    char shown[WRITE_PREVIEW_PATH + 8];
    tool_short_path(resolved, cwd, sp, sizeof sp);
    preview_sanitize_line(sp, strlen(sp), safe, sizeof safe, PV_REPLACE, NULL);
    elide_path(safe, shown, sizeof shown);

    size_t lines = count_lines(content, content_len);
    size_t ctrl = preview_control_count(content, content_len);
    Buf b;
    buf_init(&b);
    if (create) {
        if (lines == 0) {
            buf_printf(&b, "create %s\n0 B, 0 lines (empty)", shown);
        } else {
            buf_printf(&b, "create %s\n%zu B, %zu line%s, mode %04o", shown,
                       content_len, lines, lines == 1 ? "" : "s",
                       (unsigned)(mode & 0777));
            if (ctrl) {
                buf_printf(&b, ", %zu ctrl", ctrl);
            }
        }
    } else {
        buf_printf(&b, "overwrite %s\n%zu B -> %zu B", shown, old_size,
                   content_len);
        if (content_len == 0) {
            buf_adds(&b, " (empty)");
        }
        if (ctrl) {
            buf_printf(&b, ", %zu ctrl", ctrl);
        }
        if (identical) {
            buf_adds(&b, ", no change");
        }
    }

    if (!identical && content_len > 0) {
        size_t pos = 0;
        size_t idx = 0;
        const char *line = NULL;
        const char *last_p = NULL;
        size_t len = 0;
        size_t last_n = 0;
        while (pos < content_len) {
            next_line(content, content_len, &pos, &line, &len);
            if (idx < 3 || (lines <= 4 && idx == 3)) {
                append_sample(&b, line, len);
            }
            last_p = line;
            last_n = len;
            idx++;
        }
        if (lines > 4) {
            buf_printf(&b, "\n... %zu line%s not shown", lines - 4,
                       lines - 4 == 1 ? "" : "s");
            append_sample(&b, last_p, last_n);
        }
    }

    size_t n = b.len;
    if (n > TOOL_ASK_TEXT_MAX - 1) {
        n = TOOL_ASK_TEXT_MAX - 1;
    }
    memcpy(out->preview, b.p, n);
    out->preview[n] = '\0';
    out->preview_len = b.len;
    buf_free(&b);
}

int tool_prep_write(const char *cwd, const char *resolved, const JDoc *d,
                    int root, ToolPrep *out, char *err, size_t err_cap) {
    char *raw_path = NULL;
    char *content = NULL;
    WritePlan *plan = NULL;
    Buf old;
    buf_init(&old);
    int fd = -1;
    int rc = TOOL_ERR;
    size_t path_len = 0;
    size_t content_len = 0;
    struct stat st;
    struct stat pst;

    raw_path = tool_arg_string(d, root, "path", "write", &path_len, err,
                               err_cap);
    if (!raw_path) {
        goto done;
    }
    content = tool_arg_string(d, root, "content", "write", &content_len, err,
                              err_cap);
    if (!content) {
        goto done;
    }

    int overwrite = 0;
    int ot = jget(d, root, "overwrite");
    if (ot >= 0) {
        int b = jbool(d, ot, -1);
        if (b < 0) {
            failf(err, err_cap,
                  "ERROR: write: overwrite must be a boolean (true or false)");
            goto done;
        }
        overwrite = b;
    }
    if (path_len > 0 && raw_path[path_len - 1] == '/') {
        failf(err, err_cap, "ERROR: write: path must not end with '/': %s",
              raw_path);
        goto done;
    }
    if (content_len > WRITE_MAX_CONTENT) {
        failf(err, err_cap,
              "ERROR: write: content too large: %zu B > %u B", content_len,
              (unsigned)WRITE_MAX_CONTENT);
        goto done;
    }
    if (fstat(out->dfd, &pst) != 0) {
        failf(err, err_cap, "ERROR: write: cannot stat parent: %s",
              strerror(errno));
        goto done;
    }

    if (fstatat(out->dfd, out->base, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT) {
            failf(err, err_cap, "ERROR: write: cannot stat %s: %s", resolved,
                  strerror(errno));
            goto done;
        }
        mode_t mode = (mode_t)(0666 & ~current_umask());
        plan = calloc(1, sizeof *plan);
        if (!plan) {
            failf(err, err_cap, "ERROR: write: out of memory");
            goto done;
        }
        plan->path = xstrdup(resolved);
        plan->content = content;
        content = NULL;
        plan->content_len = content_len;
        plan->mode = mode;
        plan->existing = 0;

        size_t rl = strlen(resolved);
        char *hb = malloc(rl + 1 + content_len);
        if (!hb) {
            failf(err, err_cap, "ERROR: write: out of memory");
            goto done;
        }
        memcpy(hb, resolved, rl);
        hb[rl] = '\0';
        memcpy(hb + rl + 1, plan->content, content_len);
        out->ident.hash = meong_siphash24(hb, rl + 1 + content_len);
        free(hb);
        out->ident.dev = (unsigned long long)pst.st_dev;
        out->ident.ino = (unsigned long long)pst.st_ino;
        out->ident.size = (unsigned long long)content_len;
        out->ident.mtime_s = 0;
        out->ident.mtime_ns = 0;
        out->ident.mode = (unsigned)mode;
        out->ident.valid = 1;
        out->ident.kind = TOOL_IDENT_CREATE;
        build_preview(cwd, resolved, 1, 0, plan->content, content_len, mode,
                      0, out);
        out->priv = plan;
        out->free_priv = plan_free;
        plan = NULL;
        rc = TOOL_OK;
        goto done;
    }

    if (S_ISLNK(st.st_mode)) {
        failf(err, err_cap,
              "ERROR: write: refusing symlink: %s; write the real path",
              resolved);
        goto done;
    }
    if (S_ISDIR(st.st_mode)) {
        failf(err, err_cap, "ERROR: write: target is a directory: %s",
              resolved);
        goto done;
    }
    if (!S_ISREG(st.st_mode)) {
        failf(err, err_cap, "ERROR: write: target is not a regular file: %s",
              resolved);
        goto done;
    }
    if (st.st_nlink > 1) {
        failf(err, err_cap, "ERROR: write: refusing hard-linked file: %s",
              resolved);
        goto done;
    }
    if (st.st_uid != geteuid() && geteuid() != 0) {
        failf(err, err_cap, "ERROR: write: refusing foreign-owned file: %s",
              resolved);
        goto done;
    }
    if (st.st_mode & (S_ISUID | S_ISGID)) {
        failf(err, err_cap, "ERROR: write: refusing setuid/setgid file: %s",
              resolved);
        goto done;
    }
    if ((unsigned long long)st.st_dev != (unsigned long long)pst.st_dev) {
        failf(err, err_cap,
              "ERROR: write: target is on a different filesystem: %s",
              resolved);
        goto done;
    }
    if (st.st_size < 0 ||
        (unsigned long long)st.st_size > WRITE_MAX_TARGET) {
        failf(err, err_cap,
              "ERROR: write: file too large: %s (%llu B > %u B)", resolved,
              (unsigned long long)st.st_size, (unsigned)WRITE_MAX_TARGET);
        goto done;
    }
    if (!overwrite) {
        char shown[WRITE_PREVIEW_PATH + 8];
        short_result(cwd, resolved, shown, sizeof shown);
        failf(err, err_cap,
              "ERROR: write: %s already exists (%llu B); read it, then pass "
              "overwrite:true to replace it",
              shown, (unsigned long long)st.st_size);
        goto done;
    }

    fd = openat(out->dfd, out->base, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        failf(err, err_cap, "ERROR: write: cannot read %s: %s", resolved,
              strerror(errno));
        goto done;
    }
    struct stat fst;
    if (fstat(fd, &fst) != 0) {
        failf(err, err_cap, "ERROR: write: cannot stat the opened file: %s",
              strerror(errno));
        goto done;
    }
    if (fst.st_dev != st.st_dev || fst.st_ino != st.st_ino ||
        fst.st_size != st.st_size ||
        fst.st_mtim.tv_sec != st.st_mtim.tv_sec ||
        fst.st_mtim.tv_nsec != st.st_mtim.tv_nsec) {
        failf(err, err_cap,
              "ERROR: write: %s changed while opening; re-read the file",
              resolved);
        goto done;
    }
    st = fst;
    if (read_all(fd, &old, err, err_cap) != 0) {
        goto done;
    }
    close(fd);
    fd = -1;
    if (old.len > 0 &&
        memchr(old.p, 0, old.len < 8192 ? old.len : 8192) != NULL) {
        failf(err, err_cap, "ERROR: write: binary file: %s", resolved);
        goto done;
    }

    plan = calloc(1, sizeof *plan);
    if (!plan) {
        failf(err, err_cap, "ERROR: write: out of memory");
        goto done;
    }
    plan->path = xstrdup(resolved);
    plan->content = content;
    content = NULL;
    plan->content_len = content_len;
    plan->old_size = (size_t)st.st_size;
    plan->mode = st.st_mode & 07777;
    plan->uid = st.st_uid;
    plan->gid = st.st_gid;
    plan->existing = 1;
    plan->identical = old.len == content_len &&
                      (content_len == 0 ||
                       memcmp(old.p, plan->content, content_len) == 0);

    out->ident.dev = (unsigned long long)st.st_dev;
    out->ident.ino = (unsigned long long)st.st_ino;
    out->ident.size = (unsigned long long)st.st_size;
    out->ident.mtime_s = (long long)st.st_mtim.tv_sec;
    out->ident.mtime_ns = (long long)st.st_mtim.tv_nsec;
    out->ident.mode = (unsigned)plan->mode;
    out->ident.hash = meong_siphash24(old.p, old.len);
    out->ident.valid = 1;
    out->ident.kind = TOOL_IDENT_EXISTING;
    build_preview(cwd, resolved, 0, (size_t)st.st_size, plan->content,
                  content_len, plan->mode, plan->identical, out);
    out->priv = plan;
    out->free_priv = plan_free;
    plan = NULL;
    rc = TOOL_OK;

done:
    if (fd >= 0) {
        close(fd);
    }
    buf_free(&old);
    if (rc != TOOL_OK && plan) {
        plan_free(plan);
    }
    free(raw_path);
    free(content);
    return rc;
}

int tool_run_write(const char *cwd, const JDoc *d, int args, void *ud,
                   ToolAuthorizeFn authorize, ToolAsk *ask,
                   const ToolPrep *prep, Buf *out, char *err, size_t err_cap) {
    (void)d;
    (void)args;
    (void)ud;
    (void)authorize;
    (void)ask;
    WritePlan *plan = prep ? prep->priv : NULL;
    if (!plan) {
        return failf(err, err_cap, "ERROR: write: no prepared plan");
    }
    char shown[WRITE_PREVIEW_PATH + 8];
    short_result(cwd, plan->path, shown, sizeof shown);

    struct stat st;
    struct stat pst;
    if (fstat(prep->dfd, &pst) != 0) {
        return failf(err, err_cap, "ERROR: write: cannot stat parent: %s",
                     strerror(errno));
    }

    if (!plan->existing) {
        int r = fstatat(prep->dfd, prep->base, &st, AT_SYMLINK_NOFOLLOW);
        if (r == 0) {
            return failf(err, err_cap,
                         "ERROR: write: %s appeared since approval; re-run "
                         "the write",
                         shown);
        }
        if (errno != ENOENT) {
            return failf(err, err_cap, "ERROR: write: cannot stat %s: %s",
                         shown, strerror(errno));
        }
        if ((unsigned long long)pst.st_dev != prep->ident.dev ||
            (unsigned long long)pst.st_ino != prep->ident.ino) {
            return failf(err, err_cap,
                         "ERROR: write: %s changed since approval; re-run "
                         "the write",
                         shown);
        }
        if (write_file_create_at(prep->dfd, prep->base, plan->content,
                                 plan->content_len, plan->mode) != 0) {
            int e = errno;
            if (e == EEXIST) {
                return failf(err, err_cap,
                             "ERROR: write: %s appeared since approval; "
                             "re-run the write",
                             shown);
            }
            return write_error(err, err_cap, shown, e);
        }
        buf_printf(out, "OK: created %s (%zu B)", shown, plan->content_len);
        return TOOL_OK;
    }

    if (fstatat(prep->dfd, prep->base, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return failf(err, err_cap,
                     "ERROR: write: %s changed since approval; re-run the "
                     "write",
                     shown);
    }
    if ((unsigned long long)st.st_dev != prep->ident.dev ||
        (unsigned long long)st.st_ino != prep->ident.ino ||
        (unsigned long long)st.st_size != prep->ident.size ||
        (long long)st.st_mtim.tv_sec != prep->ident.mtime_s ||
        (long long)st.st_mtim.tv_nsec != prep->ident.mtime_ns ||
        (unsigned)(st.st_mode & 07777) != prep->ident.mode) {
        return failf(err, err_cap,
                     "ERROR: write: %s changed since approval; re-run the "
                     "write",
                     shown);
    }
    if (plan->identical) {
        buf_printf(out, "OK: unchanged %s (%zu B)", shown, plan->content_len);
        return TOOL_OK;
    }
    if (write_file_replace_at_keyed(prep->dfd, prep->base, plan->content,
                                    plan->content_len, plan->mode, plan->uid,
                                    plan->gid) != 0) {
        return write_error(err, err_cap, shown, errno);
    }
    buf_printf(out, "OK: overwrote %s (%zu -> %zu B)", shown, plan->old_size,
               plan->content_len);
    return TOOL_OK;
}
