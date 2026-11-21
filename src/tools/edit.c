/*
 * edit: the first mutating tool. prepare() reads the file, proves the match is
 * unique, builds the exact new bytes, the preview and the content ticket;
 * run() re-checks the ticket and writes temp+fsync+rename. It never calls
 * authorize: the layer already gated the call before prepare.
 */
#include "tools/edit.h"

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

#define EDIT_MAX_FILE (1u << 20)

typedef struct {
    char *path;
    char *new_content;
    size_t new_len;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    size_t nrep;
    size_t line_start;
    size_t line_end;
    size_t m_lines;
    size_t n_lines;
} EditPlan;

static void plan_free(void *p) {
    EditPlan *plan = p;
    if (!plan) {
        return;
    }
    free(plan->path);
    free(plan->new_content);
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

/*
 * Preview text only. The shared sanitizer owns the control/ESC/bidi table;
 * edit keeps its original flags so the shipped card bytes do not move.
 *
 * TODO(fix): PV_ELLIPSIS only drops a tab from the card (`\tfoo` -> `foo`),
 * so the human can approve a diff whose indentation looks different. write
 * passes PV_TABS; add it here and a card test that asserts the four-space
 * render. The written bytes are already exact; this is preview-only.
 */
static size_t sanitize_line(const char *s, size_t n, char *out, size_t cap) {
    return preview_sanitize_line(s, n, out, cap, PV_ELLIPSIS, NULL);
}

#define PREVIEW_LINE_MAX 256
#define PREVIEW_DIFF_LINES 3

typedef struct {
    const char *p;
    size_t n;
} EditLine;

static size_t split_lines(const char *s, size_t n, EditLine *out, size_t max) {
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            if (count < max) {
                out[count].p = s + start;
                out[count].n = i - start;
            }
            count++;
            start = i + 1;
        }
    }
    if (start < n) {
        if (count < max) {
            out[count].p = s + start;
            out[count].n = n - start;
        }
        count++;
    }
    return count;
}

static int line_eq(EditLine a, EditLine b) {
    return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}

static void pv_put(char *buf, size_t cap, size_t *len, const char *s, size_t n,
                   int *trunc) {
    if (*trunc) {
        return;
    }
    if (*len + n + 2 > cap) {
        if (*len && *len + 4 <= cap) {
            buf[(*len)++] = '\n';
            memcpy(buf + *len, "...", 3);
            *len += 3;
            buf[*len] = '\0';
        }
        *trunc = 1;
        return;
    }
    if (*len) {
        buf[(*len)++] = '\n';
    }
    memcpy(buf + *len, s, n);
    *len += n;
    buf[*len] = '\0';
}

/*
 * Preview the changed hunk, not just the first line: common prefix/suffix
 * lines are stripped, then up to PREVIEW_DIFF_LINES removed and added lines
 * are shown with a `... (+N lines)` marker. The whole thing is capped at the
 * 512-byte ToolAsk field; a truncated preview sets preview_len to the cap so
 * the card can say so. Each line is sanitized independently.
 */
static void build_preview(const char *path, const char *old, size_t old_len,
                          const char *new, size_t new_len, size_t nrep,
                          size_t *line_start, size_t *m_lines,
                          size_t *n_lines, char *buf, size_t cap,
                          size_t *out_len, int *trunc) {
    EditLine ol[PREVIEW_LINE_MAX];
    EditLine nl[PREVIEW_LINE_MAX];
    size_t on = split_lines(old, old_len, ol, PREVIEW_LINE_MAX);
    size_t nn = split_lines(new, new_len, nl, PREVIEW_LINE_MAX);
    int fallback = on > PREVIEW_LINE_MAX || nn > PREVIEW_LINE_MAX;
    size_t prefix = 0;
    size_t suffix = 0;
    if (!fallback) {
        while (prefix < on && prefix < nn &&
               line_eq(ol[prefix], nl[prefix])) {
            prefix++;
        }
        while (suffix < on - prefix && suffix < nn - prefix &&
               line_eq(ol[on - 1 - suffix], nl[nn - 1 - suffix])) {
            suffix++;
        }
    }
    size_t removed = on - prefix - suffix;
    size_t added = nn - prefix - suffix;
    *line_start += prefix;
    *m_lines = removed;
    *n_lines = added;

    char safe_path[160];
    sanitize_line(path, strlen(path), safe_path, sizeof safe_path);
    const char *shown = safe_path;
    size_t plen = strlen(shown);
    if (plen > 96) {
        shown += plen - 96;
    }
    char head[256];
    int h = snprintf(head, sizeof head, "%s:%zu", shown, *line_start);
    if (h > 0 && (size_t)h < sizeof head) {
        if (nrep > 1) {
            snprintf(head + h, sizeof head - h, " x%zu", nrep);
            h = (int)strlen(head);
        }
        if (removed > 1 || added > 1) {
            snprintf(head + h, sizeof head - h, " -%zu +%zu", removed, added);
        }
    }

    size_t len = 0;
    *trunc = 0;
    pv_put(buf, cap, &len, head, strlen(head), trunc);
    size_t show = removed < PREVIEW_DIFF_LINES ? removed : PREVIEW_DIFF_LINES;
    for (size_t i = 0; i < show; i++) {
        char line[192];
        line[0] = '-';
        size_t z = sanitize_line(ol[prefix + i].p, ol[prefix + i].n, line + 1,
                                 sizeof line - 1);
        pv_put(buf, cap, &len, line, z + 1, trunc);
    }
    if (removed > show) {
        char more[64];
        snprintf(more, sizeof more, "-... (+%zu lines)", removed - show);
        pv_put(buf, cap, &len, more, strlen(more), trunc);
    }
    show = added < PREVIEW_DIFF_LINES ? added : PREVIEW_DIFF_LINES;
    for (size_t i = 0; i < show; i++) {
        char line[192];
        line[0] = '+';
        size_t z = sanitize_line(nl[prefix + i].p, nl[prefix + i].n, line + 1,
                                 sizeof line - 1);
        pv_put(buf, cap, &len, line, z + 1, trunc);
    }
    if (added > show) {
        char more[64];
        snprintf(more, sizeof more, "+... (+%zu lines)", added - show);
        pv_put(buf, cap, &len, more, strlen(more), trunc);
    }
    *out_len = *trunc ? cap : len;
}

static size_t find_matches(const char *hay, size_t hlen, const char *needle,
                           size_t nlen, size_t *lines, size_t max_lines,
                           size_t *first_off) {
    if (nlen == 0) {
        return 0;
    }
    size_t count = 0;
    size_t i = 0;
    size_t scanned = 0;
    size_t line = 1;
    while (i + nlen <= hlen) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            for (size_t j = scanned; j < i; j++) {
                if (hay[j] == '\n') {
                    line++;
                }
            }
            if (count < max_lines) {
                lines[count] = line;
            }
            if (count == 0 && first_off) {
                *first_off = i;
            }
            count++;
            i += nlen;
            scanned = i;
        } else {
            i++;
        }
    }
    return count;
}

static int read_all(int fd, Buf *b, char *err, size_t cap) {
    char tmp[8192];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return failf(err, cap, "ERROR: edit: read failed: %s",
                         strerror(errno));
        }
        if (n == 0) {
            return 0;
        }
        if (b->len + (size_t)n > EDIT_MAX_FILE) {
            return failf(err, cap,
                         "ERROR: edit: file too large: over %u B",
                         (unsigned)EDIT_MAX_FILE);
        }
        buf_add(b, tmp, (size_t)n);
    }
}

int tool_prep_edit(const char *cwd, const char *resolved, const JDoc *d,
                   int root, ToolPrep *out, char *err, size_t err_cap) {
    (void)cwd;
    (void)resolved;
    size_t path_len = 0;
    size_t old_len = 0;
    size_t new_len = 0;
    char *path = tool_arg_string(d, root, "path", "edit", &path_len, err,
                                 err_cap);
    if (!path) {
        return TOOL_ERR;
    }
    char *old = tool_arg_string(d, root, "old_string", "edit", &old_len, err,
                                err_cap);
    if (!old) {
        free(path);
        return TOOL_ERR;
    }
    char *new = tool_arg_string(d, root, "new_string", "edit", &new_len, err,
                                err_cap);
    if (!new) {
        free(path);
        free(old);
        return TOOL_ERR;
    }
    int replace_all = jbool(d, jget(d, root, "replace_all"), 0);
    EditPlan *plan = NULL;
    Buf body;
    buf_init(&body);
    int fd = -1;
    int rc = TOOL_ERR;

    if (old_len == 0) {
        failf(err, err_cap, "ERROR: edit: old_string must not be empty");
        goto done;
    }
    if (old_len == new_len && memcmp(old, new, old_len) == 0) {
        failf(err, err_cap,
              "ERROR: edit: old_string and new_string are identical; "
              "nothing to do");
        goto done;
    }

    struct stat st;
    if (fstatat(out->dfd, out->base, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        failf(err, err_cap, "ERROR: edit: cannot stat %s: %s", path,
              strerror(errno));
        goto done;
    }
    if (S_ISLNK(st.st_mode)) {
        failf(err, err_cap, "ERROR: edit: refusing symlink: %s", path);
        goto done;
    }
    if (!S_ISREG(st.st_mode)) {
        failf(err, err_cap, "ERROR: edit: not a regular file: %s", path);
        goto done;
    }
    if (st.st_nlink > 1) {
        failf(err, err_cap, "ERROR: edit: refusing hard-linked file: %s",
              path);
        goto done;
    }
    if (st.st_uid != geteuid() && geteuid() != 0) {
        failf(err, err_cap, "ERROR: edit: refusing foreign-owned file: %s",
              path);
        goto done;
    }
    if (st.st_mode & (S_ISUID | S_ISGID)) {
        failf(err, err_cap, "ERROR: edit: refusing setuid/setgid file: %s",
              path);
        goto done;
    }
    if (st.st_size < 0 || (unsigned long long)st.st_size > EDIT_MAX_FILE) {
        failf(err, err_cap,
              "ERROR: edit: file too large: %s (%llu B > %u B)", path,
              (unsigned long long)st.st_size, (unsigned)EDIT_MAX_FILE);
        goto done;
    }

    fd = openat(out->dfd, out->base, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        failf(err, err_cap, "ERROR: edit: cannot open %s: %s", path,
              strerror(errno));
        goto done;
    }
    struct stat fst;
    if (fstat(fd, &fst) != 0) {
        failf(err, err_cap, "ERROR: edit: cannot stat the opened file: %s",
              strerror(errno));
        goto done;
    }
    if (fst.st_dev != st.st_dev || fst.st_ino != st.st_ino ||
        fst.st_size != st.st_size ||
        fst.st_mtim.tv_sec != st.st_mtim.tv_sec ||
        fst.st_mtim.tv_nsec != st.st_mtim.tv_nsec) {
        failf(err, err_cap,
              "ERROR: edit: %s changed while opening; re-read the file",
              path);
        goto done;
    }
    st = fst;
    if (read_all(fd, &body, err, err_cap) != 0) {
        goto done;
    }
    if (body.len > 0 && memchr(body.p, 0, body.len < 8192 ? body.len : 8192)) {
        failf(err, err_cap, "ERROR: edit: binary file: %s", path);
        goto done;
    }

    size_t lines[8] = {0};
    size_t first_off = 0;
    size_t nrep = find_matches(body.p, body.len, old, old_len, lines, 8,
                               &first_off);
    if (nrep == 0) {
        const char *hint = "";
        int has_crlf = 0;
        for (size_t i = 0; i + 1 < body.len; i++) {
            if (body.p[i] == '\r' && body.p[i + 1] == '\n') {
                has_crlf = 1;
                break;
            }
        }
        int old_has_cr = memchr(old, '\r', old_len) != NULL;
        char applied[64] = "";
        size_t app_off = 0;
        if (find_matches(body.p, body.len, new, new_len, NULL, 0, &app_off) ==
            1) {
            size_t line = 1;
            for (size_t i = 0; i < app_off; i++) {
                if (body.p[i] == '\n') {
                    line++;
                }
            }
            snprintf(applied, sizeof applied,
                     "; the replacement text is already at line %zu", line);
        }
        if (has_crlf && !old_has_cr) {
            hint = "; the file uses CRLF, include the \\r";
        }
        failf(err, err_cap,
              "ERROR: edit: old_string not found in %s%s%s; re-read the file",
              path, hint, applied);
        (void)hint;
        goto done;
    }
    if (nrep > 1 && !replace_all) {
        char where[128];
        size_t w = 0;
        w += (size_t)snprintf(where + w, sizeof where - w, " at lines");
        for (size_t i = 0; i < nrep && i < 8; i++) {
            int k = snprintf(where + w, sizeof where - w, "%s%zu",
                             i ? ", " : " ", lines[i]);
            if (k < 0 || (size_t)k >= sizeof where - w) {
                break;
            }
            w += (size_t)k;
        }
        if (nrep > 8) {
            snprintf(where + w, sizeof where - w, ", ...");
        }
        failf(err, err_cap,
              "ERROR: edit: old_string matches %zu times in %s%s; add "
              "context or set replace_all",
              nrep, path, where);
        goto done;
    }

    if (new_len > old_len) {
        size_t delta = new_len - old_len;
        if (nrep > (EDIT_MAX_FILE - body.len) / delta) {
            failf(err, err_cap, "ERROR: edit: result would exceed %u B",
                  (unsigned)EDIT_MAX_FILE);
            goto done;
        }
    }
    size_t out_len = body.len - nrep * old_len + nrep * new_len;
    if (out_len > EDIT_MAX_FILE) {
        failf(err, err_cap, "ERROR: edit: result would exceed %u B",
              (unsigned)EDIT_MAX_FILE);
        goto done;
    }
    char *content = xmalloc(out_len + 1);
    size_t o = 0;
    size_t pos = 0;
    size_t i = 0;
    size_t done_count = 0;
    while (i + old_len <= body.len && done_count < nrep) {
        if (memcmp(body.p + i, old, old_len) == 0) {
            memcpy(content + o, body.p + pos, i - pos);
            o += i - pos;
            memcpy(content + o, new, new_len);
            o += new_len;
            i += old_len;
            pos = i;
            done_count++;
        } else {
            i++;
        }
    }
    memcpy(content + o, body.p + pos, body.len - pos);
    o += body.len - pos;
    content[o] = '\0';

    plan = calloc(1, sizeof *plan);
    if (!plan) {
        free(content);
        failf(err, err_cap, "ERROR: edit: out of memory");
        goto done;
    }
    plan->path = path;
    path = NULL;
    plan->new_content = content;
    plan->new_len = o;
    plan->mode = st.st_mode & 07777;
    plan->uid = st.st_uid;
    plan->gid = st.st_gid;
    plan->nrep = nrep;

    size_t line_start = 1;
    for (size_t j = 0; j < first_off; j++) {
        if (body.p[j] == '\n') {
            line_start++;
        }
    }
    plan->line_start = line_start;
    int trunc = 0;
    build_preview(plan->path, old, old_len, new, new_len, nrep,
                  &plan->line_start, &plan->m_lines, &plan->n_lines,
                  out->preview, sizeof out->preview, &out->preview_len,
                  &trunc);
    plan->line_end =
        plan->line_start + (plan->n_lines ? plan->n_lines - 1 : 0);
    (void)trunc;

    out->ident.dev = st.st_dev;
    out->ident.ino = st.st_ino;
    out->ident.size = (unsigned long long)st.st_size;
    out->ident.mtime_s = (long long)st.st_mtim.tv_sec;
    out->ident.mtime_ns = (long long)st.st_mtim.tv_nsec;
    out->ident.mode = (unsigned)plan->mode;
    out->ident.hash = meong_siphash24(body.p, body.len);
    out->ident.valid = 1;
    out->ident.kind = TOOL_IDENT_EXISTING;

    out->priv = plan;
    out->free_priv = plan_free;
    plan = NULL;
    rc = TOOL_OK;

done:
    if (fd >= 0) {
        close(fd);
    }
    buf_free(&body);
    if (plan) {
        plan_free(plan);
    }
    free(path);
    free(old);
    free(new);
    return rc;
}

static void append_excerpt(Buf *out, const char *content, size_t len,
                           size_t start, size_t end) {
    size_t line = 1;
    size_t i = 0;
    int shown = 0;
    while (i < len && line < start) {
        if (content[i] == '\n') {
            line++;
        }
        i++;
    }
    while (i < len && line <= end && shown < 6 && out->len < 700) {
        size_t e = i;
        while (e < len && content[e] != '\n') {
            e++;
        }
        buf_printf(out, "%zu: %.*s\n", line, (int)(e - i), content + i);
        shown++;
        i = e < len ? e + 1 : e;
        line++;
    }
}

int tool_run_edit(const char *cwd, const JDoc *d, int args, void *ud,
                  ToolAuthorizeFn authorize, ToolAsk *ask,
                  const ToolPrep *prep, Buf *out, char *err, size_t err_cap) {
    (void)cwd;
    (void)d;
    (void)args;
    (void)ud;
    (void)authorize;
    (void)ask;
    EditPlan *plan = prep ? prep->priv : NULL;
    if (!plan) {
        return failf(err, err_cap, "ERROR: edit: no prepared plan");
    }
    struct stat st;
    if (fstatat(prep->dfd, prep->base, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        (unsigned long long)st.st_dev != prep->ident.dev ||
        (unsigned long long)st.st_ino != prep->ident.ino ||
        (unsigned long long)st.st_size != prep->ident.size ||
        (long long)st.st_mtim.tv_sec != prep->ident.mtime_s ||
        (long long)st.st_mtim.tv_nsec != prep->ident.mtime_ns ||
        (unsigned)(st.st_mode & 07777) != prep->ident.mode) {
        return failf(err, err_cap,
                     "ERROR: edit: %s changed since approval; re-run the edit",
                     plan->path);
    }
    if (write_file_replace_at(prep->dfd, prep->base, plan->new_content,
                              plan->new_len, plan->mode, plan->uid,
                              plan->gid) != 0) {
        return failf(err, err_cap, "ERROR: edit: write failed: %s",
                     strerror(errno));
    }
    buf_printf(out, "OK: edited %s:%zu-%zu (+%zu -%zu)", plan->path,
               plan->line_start, plan->line_end, plan->n_lines,
               plan->m_lines);
    if (plan->nrep > 1) {
        buf_printf(out, " x%zu", plan->nrep);
    }
    buf_addc(out, '\n');
    if (plan->n_lines > 0) {
        append_excerpt(out, plan->new_content, plan->new_len, plan->line_start,
                       plan->line_end);
    }
    return TOOL_OK;
}
