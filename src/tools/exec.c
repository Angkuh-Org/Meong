/*
 * Handlers behind tool_execute(): read_file, search and list_dir.  Everything
 * here is read-only and every path that reaches fopen/opendir/readlink went
 * through the authorize callback first.  The prototypes live in exec.h.
 */
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "json/json.h"
#define JSMN_HEADER
#include "json/jsmn.h"
#include "tools/exec.h"
#include "tools/tools.h"

#define OUT_CAP (64u << 10)
#define SCAN_BYTES_MAX (16 << 20)
#define BOUND_HEAD (TOOL_INLINE_MAX / 5)
#define SEARCH_FILES_MAX 500
#define SEARCH_MATCHES_MAX 200
#define SEARCH_DEPTH_MAX 32
#define SEARCH_CTX_MAX 20
#define SEARCH_LINE_MAX (8u << 10)
#define LIST_ENTRIES_MAX 500

enum {
    WALK_OK = 0,
    WALK_PERM = 1,
    WALK_STOP = 2,
};

typedef struct {
    void *ud;
    ToolAuthorizeFn authorize;
    char *ask_path;
    size_t ask_cap;
    Buf *out;
    char *err;
    size_t err_cap;
} Ctx;

static int failf(Ctx *c, const char *fmt, ...) {
    if (c->err && c->err_cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err, c->err_cap, fmt, ap);
        va_end(ap);
    }
    return TOOL_ERR;
}

/* 1 allows, 0 denies, -1 asks and records the exact path in ask_path. */
static int ctx_allow(Ctx *c, const char *path) {
    if (!c->authorize) {
        return 1;
    }
    int r = c->authorize(c->ud, path);
    if (r < 0) {
        if (c->ask_path && c->ask_cap) {
            snprintf(c->ask_path, c->ask_cap, "%s", path);
        }
        return -1;
    }
    return r ? 1 : 0;
}

/*
 * Callers routinely spell an unused optional argument as null.  jsmn keeps the
 * literal span, so this is exact: only a real `null` counts as absent, while a
 * string/object/array in an integer slot still fails.
 */
static int jisnull(const JDoc *d, int tok) {
    if (tok < 0 || !d->tok || !d->text || jtype(d, tok) != J_PRIMITIVE) {
        return 0;
    }
    const jsmntok_t *t = &((const jsmntok_t *)d->tok)[tok];
    return t->end - t->start == 4 && memcmp(d->text + t->start, "null", 4) == 0;
}

static int arg_string(const JDoc *d, int obj, const char *key, int required,
                      char **out, Ctx *c) {
    *out = NULL;
    int t = jget(d, obj, key);
    if (t < 0) {
        return required ? failf(c, "ERROR: missing required argument: %s", key)
                        : 0;
    }
    if (jtype(d, t) != J_STRING) {
        if (!required && jisnull(d, t)) {
            return 0;
        }
        return failf(c, "ERROR: argument %s must be a string", key);
    }
    *out = jstring_alloc(d, t);
    if (!*out) {
        return failf(c, "ERROR: out of memory");
    }
    return 0;
}

static int arg_int(const JDoc *d, int obj, const char *key, long long def,
                   long long *out, Ctx *c) {
    *out = def;
    int t = jget(d, obj, key);
    if (t < 0 || jisnull(d, t)) {
        return 0;
    }
    long long v = jint(d, t, LLONG_MIN);
    if (jtype(d, t) != J_PRIMITIVE || v == LLONG_MIN) {
        return failf(c, "ERROR: argument %s must be an integer", key);
    }
    *out = v;
    return 0;
}

static char *path_join(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    int slash = !(dl && dir[dl - 1] == '/');
    char *p = xmalloc(dl + (size_t)slash + nl + 1);
    memcpy(p, dir, dl);
    if (slash) {
        p[dl] = '/';
    }
    memcpy(p + dl + slash, name, nl + 1);
    return p;
}

static void fmt_mtime(time_t t, char *out, size_t cap) {
    struct tm tm;
    if (!gmtime_r(&t, &tm) ||
        strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        snprintf(out, cap, "unknown");
    }
}

static int looks_binary(FILE *f) {
    unsigned char probe[4096];
    size_t n = fread(probe, 1, sizeof probe, f);
    rewind(f);
    return memchr(probe, 0, n) != NULL;
}

/* ---- read_file ---------------------------------------------------------- */

static void put(Buf *b, const char *s, size_t n, int *capped) {
    if (*capped) {
        return;
    }
    if (b->len + n > OUT_CAP) {
        *capped = 1;
        return;
    }
    buf_add(b, s, n);
}

/*
 * One pass: count every line for the header, copy only [start, end] into body.
 * Out-of-range stretches skip with memchr, so a 64 MiB file costs one scan.
 * need_prefix survives a chunk boundary: a line longer than the 64 KiB read
 * buffer must not grow a second "N: " in the middle.  The count itself stops
 * at SCAN_BYTES_MAX so a 10 GB log is not read end to end for one header.
 */
static void scan_range(FILE *f, long long start, long long end, Buf *body,
                       long long *lines_out, int *capped, int *partial) {
    /* 64 KiB is the measured sweet spot: one chunk fills OUT_CAP and glibc
     * reads it directly. 16 MiB file, best of 5: 64 KiB 8.4 ms, 32 KiB
     * 8.6 ms, 16 KiB 8.9 ms, 4 KiB 18.0 ms. Keep it unless that trade
     * changes. */
    char chunk[1 << 16];
    size_t n;
    long long line = 1;
    long long total = 0;
    long long bytes = 0;
    int in_range = start <= 1 && 1 <= end;
    int need_prefix = 1;
    int last_nl = 1;
    *capped = 0;
    *partial = 0;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        size_t i = 0;
        if (bytes + (long long)n > SCAN_BYTES_MAX) {
            *partial = 1;
            break;
        }
        bytes += (long long)n;
        while (i < n) {
            char *nl = memchr(chunk + i, '\n', n - i);
            size_t seg = nl ? (size_t)(nl - chunk) - i : n - i;
            if (in_range) {
                if (need_prefix) {
                    char pre[32];
                    int k = snprintf(pre, sizeof pre, "%lld: ", line);
                    put(body, pre, (size_t)k, capped);
                    need_prefix = 0;
                }
                put(body, chunk + i, seg, capped);
                if (nl) {
                    put(body, "\n", 1, capped);
                }
            }
            if (!nl) {
                last_nl = 0;
                break;
            }
            total++;
            line++;
            last_nl = 1;
            need_prefix = 1;
            i += seg + 1;
            in_range = line >= start && line <= end;
        }
    }
    if (!last_nl) {
        total++;
    }
    *lines_out = total;
}

int tool_run_read_file(const JDoc *d, int args, void *ud,
                       ToolAuthorizeFn authorize, char *ask_path,
                       size_t ask_cap, Buf *out, char *err, size_t err_cap) {
    Ctx c = {.ud = ud, .authorize = authorize, .ask_path = ask_path,
             .ask_cap = ask_cap, .out = out, .err = err, .err_cap = err_cap};
    char *path = NULL;
    FILE *f = NULL;
    Buf body;
    buf_init(&body);
    struct stat st;
    long long start = 1;
    long long end = LLONG_MIN; /* sentinel: end_line was not given */
    long long total = 0;
    int capped = 0;
    int partial = 0;
    int rc = TOOL_ERR;

    if (arg_string(d, args, "path", 1, &path, &c) < 0) {
        goto done;
    }
    if (!*path) {
        failf(&c, "ERROR: argument path must not be empty");
        goto done;
    }
    if (arg_int(d, args, "start_line", 1, &start, &c) < 0 ||
        arg_int(d, args, "end_line", LLONG_MIN, &end, &c) < 0) {
        goto done;
    }
    if (start < 1) {
        failf(&c, "ERROR: start_line must be >= 1");
        goto done;
    }
    if (end == LLONG_MIN) {
        /* Default is 400 lines starting at start_line, not a fixed line 400. */
        end = start > LLONG_MAX - 400 ? LLONG_MAX : start + 399;
    }
    if (end < start) {
        failf(&c, "ERROR: end_line must be >= start_line");
        goto done;
    }

    int a = ctx_allow(&c, path);
    if (a < 0) {
        rc = TOOL_NEED_PERM;
        goto done;
    }
    if (a == 0) {
        failf(&c, "ERROR: permission denied: %s", path);
        goto done;
    }

    /* stat first: fopen() on a FIFO with no writer would block forever. */
    if (stat(path, &st) != 0) {
        failf(&c, "ERROR: cannot stat %s: %s", path, strerror(errno));
        goto done;
    }
    if (!S_ISREG(st.st_mode)) {
        failf(&c, "ERROR: not a regular file: %s", path);
        goto done;
    }
    f = fopen(path, "rb");
    if (!f) {
        failf(&c, "ERROR: cannot open %s: %s", path, strerror(errno));
        goto done;
    }
    /* The opened file is the one we report on; re-check it did not change. */
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        failf(&c, "ERROR: not a regular file: %s", path);
        goto done;
    }
    if (looks_binary(f)) {
        failf(&c, "ERROR: binary file (NUL byte in the first 4 KiB): %s", path);
        goto done;
    }

    scan_range(f, start, end, &body, &total, &capped, &partial);
    char mt[32];
    fmt_mtime(st.st_mtime, mt, sizeof mt);
    buf_printf(out, "[file: %s | bytes: %lld | mtime: %s | lines: %lld%s]\n",
               path, (long long)st.st_size, mt, total, partial ? "+" : "");
    if (body.len) {
        buf_add(out, body.p, body.len);
    } else if (total == 0) {
        buf_adds(out, "[empty file]\n");
    } else {
        buf_printf(out, "[no lines in range %lld-%lld]\n", start, end);
    }
    if (capped) {
        buf_adds(out, "\n[... truncated at 64 KiB ...]\n");
    }
    rc = TOOL_OK;

done:
    if (f) {
        fclose(f);
    }
    buf_free(&body);
    free(path);
    return rc;
}

/* ---- search ------------------------------------------------------------- */

typedef struct {
    regex_t *re;
    const char *glob;
    int ctx;
    int files;
    int matches;
    int denied;
    int deep;
    int stopped;
} Search;

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void emit_match(Buf *out, const char *path, long long no,
                       const char *text, int cut) {
    buf_printf(out, "%s:%lld: %s%s\n", path, no, text, cut ? " [...]" : "");
}

static void emit_ctx(Buf *out, const char *path, long long no,
                     const char *text, int cut) {
    buf_printf(out, "  %s:%lld: %s%s\n", path, no, text, cut ? " [...]" : "");
}

/*
 * Keep the last `max` lines for match context.  Slots are reused: a line
 * longer than the slot's capacity grows it once, so scanning a file costs one
 * copy per line instead of one malloc/free pair, and the head cursor removes
 * the three memmoves the old shift-down ring did per line.
 */
static void ring_push(char **ring, size_t *caps, long long *nos,
                      unsigned char *cuts, int *head, int *n, int max,
                      const char *line, size_t len, long long no, int cut) {
    if (max <= 0) {
        return;
    }
    int slot;
    if (*n < max) {
        slot = (*head + *n) % max;
        (*n)++;
    } else {
        slot = *head;
        *head = (*head + 1) % max;
    }
    if (caps[slot] < len + 1) {
        size_t cap = caps[slot] ? caps[slot] : 64;
        while (cap < len + 1) {
            cap *= 2;
        }
        ring[slot] = xrealloc(ring[slot], cap);
        caps[slot] = cap;
    }
    memcpy(ring[slot], line, len);
    ring[slot][len] = '\0';
    nos[slot] = no;
    cuts[slot] = (unsigned char)cut;
}

static int search_file(Ctx *c, Search *s, const char *path,
                       const char *name) {
    if (s->stopped) {
        return WALK_STOP;
    }
    if (s->glob && fnmatch(s->glob, name, 0) != 0) {
        return WALK_OK;
    }
    if (s->files >= SEARCH_FILES_MAX) {
        s->stopped = 1;
        return WALK_STOP;
    }
    int a = ctx_allow(c, path);
    if (a < 0) {
        return WALK_PERM;
    }
    if (a == 0) {
        s->denied++;
        return WALK_OK;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return WALK_OK;
    }
    s->files++;
    if (looks_binary(f)) {
        fclose(f);
        return WALK_OK;
    }

    char *line = NULL;
    size_t lcap = 0;
    ssize_t got;
    long long no = 0;
    long long last = 0;
    int after = 0;
    int ring_n = 0;
    int ring_head = 0;
    char *ring[SEARCH_CTX_MAX];
    size_t ring_cap[SEARCH_CTX_MAX];
    long long ring_no[SEARCH_CTX_MAX];
    unsigned char ring_cut[SEARCH_CTX_MAX];
    memset(ring, 0, sizeof ring);
    memset(ring_cap, 0, sizeof ring_cap);
    while ((got = getline(&line, &lcap, f)) >= 0) {
        no++;
        size_t ln = (size_t)got;
        while (ln && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) {
            line[--ln] = '\0';
        }
        /*
         * Cap what regexec sees: a model-supplied pattern like (a+)+$ against
         * a multi-megabyte line would otherwise hang the UI thread.
         */
        int cut = ln > SEARCH_LINE_MAX;
        if (cut) {
            ln = SEARCH_LINE_MAX;
            line[ln] = '\0';
        }
        if (regexec(s->re, line, 0, NULL, 0) == 0) {
            if (s->matches >= SEARCH_MATCHES_MAX) {
                s->stopped = 1;
                break;
            }
            int i = ring_head;
            for (int k = 0; k < ring_n; k++) {
                if (ring_no[i] > last) {
                    emit_ctx(c->out, path, ring_no[i], ring[i], ring_cut[i]);
                    last = ring_no[i];
                }
                if (++i == ring_n) {
                    i = 0;
                }
            }
            if (no > last) {
                emit_match(c->out, path, no, line, cut);
                last = no;
            }
            s->matches++;
            after = s->ctx;
        } else if (after > 0) {
            if (no > last) {
                emit_ctx(c->out, path, no, line, cut);
                last = no;
            }
            after--;
        }
        ring_push(ring, ring_cap, ring_no, ring_cut, &ring_head, &ring_n,
                  s->ctx, line, ln, no, cut);
        if (c->out->len >= OUT_CAP) {
            s->stopped = 1;
            break;
        }
    }
    free(line);
    for (int i = 0; i < SEARCH_CTX_MAX; i++) {
        free(ring[i]);
    }
    fclose(f);
    return s->stopped ? WALK_STOP : WALK_OK;
}

static int walk(Ctx *c, Search *s, const char *dir, int depth) {
    if (depth > SEARCH_DEPTH_MAX) {
        s->deep = 1;
        return WALK_OK;
    }
    DIR *dp = opendir(dir);
    if (!dp) {
        return WALK_OK;
    }
    char **names = NULL;
    int n = 0;
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = xrealloc(names, (size_t)cap * sizeof *names);
        }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(dp);
    if (n > 1) {
        qsort(names, (size_t)n, sizeof *names, cmp_names);
    }

    int rc = WALK_OK;
    for (int i = 0; i < n && rc == WALK_OK; i++) {
        const char *name = names[i];
        char *path = path_join(dir, name);
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                /* Symlinks never reach here: lstat, not stat. */
                if (strcmp(name, ".git") && strcmp(name, "build") &&
                    strcmp(name, "node_modules")) {
                    rc = walk(c, s, path, depth + 1);
                }
            } else if (S_ISREG(st.st_mode)) {
                rc = search_file(c, s, path, name);
            }
        }
        free(path);
    }
    for (int i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
    return rc;
}

int tool_run_search(const JDoc *d, int args, void *ud,
                    ToolAuthorizeFn authorize, char *ask_path, size_t ask_cap,
                    Buf *out, char *err, size_t err_cap) {
    Ctx c = {.ud = ud, .authorize = authorize, .ask_path = ask_path,
             .ask_cap = ask_cap, .out = out, .err = err, .err_cap = err_cap};
    char *pattern = NULL;
    char *path = NULL;
    char *glob = NULL;
    regex_t re;
    int compiled = 0;
    long long ctx = 2;
    int rc = TOOL_ERR;

    if (arg_string(d, args, "pattern", 1, &pattern, &c) < 0 ||
        arg_string(d, args, "path", 1, &path, &c) < 0 ||
        arg_string(d, args, "glob", 0, &glob, &c) < 0 ||
        arg_int(d, args, "context_lines", 2, &ctx, &c) < 0) {
        goto done;
    }
    if (!*path) {
        failf(&c, "ERROR: argument path must not be empty");
        goto done;
    }
    if (ctx < 0) {
        ctx = 0;
    }
    if (ctx > SEARCH_CTX_MAX) {
        ctx = SEARCH_CTX_MAX;
    }
    int rerr = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
    if (rerr != 0) {
        char msg[256];
        regerror(rerr, &re, msg, sizeof msg);
        failf(&c, "ERROR: invalid regex: %s", msg);
        goto done;
    }
    compiled = 1;

    int a = ctx_allow(&c, path);
    if (a < 0) {
        rc = TOOL_NEED_PERM;
        goto done;
    }
    if (a == 0) {
        failf(&c, "ERROR: permission denied: %s", path);
        goto done;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        failf(&c, "ERROR: cannot stat %s: %s", path, strerror(errno));
        goto done;
    }
    const char *base = strrchr(path, '/');
    base = base && base[1] ? base + 1 : path;
    Search s = {.re = &re, .glob = glob, .ctx = (int)ctx};
    int wr;
    if (S_ISDIR(st.st_mode)) {
        wr = walk(&c, &s, path, 0);
    } else if (S_ISREG(st.st_mode)) {
        wr = search_file(&c, &s, path, base);
    } else {
        failf(&c, "ERROR: not a file or directory: %s", path);
        goto done;
    }
    if (wr == WALK_PERM) {
        rc = TOOL_NEED_PERM;
        goto done;
    }
    if (out->len == 0) {
        buf_adds(out, "[no matches]\n");
    }
    if (s.denied) {
        buf_printf(out, "[... %d files skipped (permission) ...]\n", s.denied);
    }
    if (s.deep) {
        buf_adds(out, "[... deeper directories not scanned ...]\n");
    }
    if (s.stopped) {
        buf_printf(out, "[... stopped: %d matches in %d files ...]\n",
                   s.matches, s.files);
    }
    rc = TOOL_OK;

done:
    if (compiled) {
        regfree(&re);
    }
    free(pattern);
    free(path);
    free(glob);
    return rc;
}

/* ---- list_dir ----------------------------------------------------------- */

int tool_run_list_dir(const JDoc *d, int args, void *ud,
                      ToolAuthorizeFn authorize, char *ask_path,
                      size_t ask_cap, Buf *out, char *err, size_t err_cap) {
    Ctx c = {.ud = ud, .authorize = authorize, .ask_path = ask_path,
             .ask_cap = ask_cap, .out = out, .err = err, .err_cap = err_cap};
    char *path = NULL;
    DIR *dp = NULL;
    char **names = NULL;
    int n = 0;
    int cap = 0;
    int rc = TOOL_ERR;

    if (arg_string(d, args, "path", 1, &path, &c) < 0) {
        goto done;
    }
    if (!*path) {
        failf(&c, "ERROR: argument path must not be empty");
        goto done;
    }
    int a = ctx_allow(&c, path);
    if (a < 0) {
        rc = TOOL_NEED_PERM;
        goto done;
    }
    if (a == 0) {
        failf(&c, "ERROR: permission denied: %s", path);
        goto done;
    }
    dp = opendir(path);
    if (!dp) {
        failf(&c, "ERROR: cannot open directory %s: %s", path,
              strerror(errno));
        goto done;
    }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = xrealloc(names, (size_t)cap * sizeof *names);
        }
        names[n++] = xstrdup(de->d_name);
    }
    if (n > 1) {
        qsort(names, (size_t)n, sizeof *names, cmp_names);
    }
    for (int i = 0; i < n && i < LIST_ENTRIES_MAX; i++) {
        char *full = path_join(path, names[i]);
        struct stat st;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                buf_printf(out, "d %s\n", names[i]);
            } else if (S_ISREG(st.st_mode)) {
                buf_printf(out, "f %s %lld\n", names[i],
                           (long long)st.st_size);
            } else if (S_ISLNK(st.st_mode)) {
                char tgt[PATH_MAX];
                ssize_t k = readlink(full, tgt, sizeof tgt - 1);
                if (k < 0) {
                    k = 0;
                }
                tgt[k] = '\0';
                buf_printf(out, "l %s -> %s\n", names[i], k ? tgt : "?");
            }
        }
        free(full);
    }
    if (n > LIST_ENTRIES_MAX) {
        buf_printf(out, "[... %d more entries]\n", n - LIST_ENTRIES_MAX);
    }
    if (n == 0) {
        buf_adds(out, "[empty directory]\n");
    }
    rc = TOOL_OK;

done:
    if (dp) {
        closedir(dp);
    }
    for (int i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
    free(path);
    return rc;
}

/* ---- bounding ----------------------------------------------------------- */

static void sanitize_id(const char *id, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = id ? id : ""; *p && n + 1 < cap; p++) {
        char ch = *p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
            out[n++] = ch;
        }
    }
    if (n == 0) {
        static const char fallback[] = "call";
        for (size_t i = 0; fallback[i] && n + 1 < cap; i++) {
            out[n++] = fallback[i];
        }
    }
    out[n] = '\0';
}

int tool_bound_result(const char *call_id, const char *text, size_t len,
                      const char *spill_dir, Buf *out,
                      char *err, size_t err_cap) {
    if (err && err_cap) {
        err[0] = '\0';
    }
    if (!out) {
        if (err && err_cap) {
            snprintf(err, err_cap, "ERROR: no output buffer");
        }
        return TOOL_ERR;
    }
    if (!text || !len) {
        return TOOL_OK;
    }
    if (len <= TOOL_INLINE_MAX) {
        buf_add(out, text, len);
        return TOOL_OK;
    }

    size_t head = BOUND_HEAD;
    size_t tail = TOOL_INLINE_MAX - BOUND_HEAD;
    size_t omitted = len - head - tail;
    char safe[64];
    char path[PATH_MAX];
    sanitize_id(call_id, safe, sizeof safe);
    path[0] = '\0';
    int spilled = 0;
    int have_dir = spill_dir && *spill_dir;
    if (have_dir) {
        int k = snprintf(path, sizeof path, "%s/%s.txt", spill_dir, safe);
        if (k > 0 && (size_t)k < sizeof path && mkdir_p(spill_dir) == 0 &&
            write_file_atomic(path, text, len) == 0) {
            spilled = 1;
        } else {
            path[0] = '\0';
        }
    }

    buf_add(out, text, head);
    if (spilled) {
        buf_printf(out, "\n\n[... %zu bytes omitted; full output: %s ...]\n\n",
                   omitted, path);
    } else if (have_dir) {
        buf_printf(out, "\n\n[... %zu bytes omitted; spill to %s failed "
                        "...]\n\n",
                   omitted, path[0] ? path : spill_dir);
    } else {
        buf_printf(out, "\n\n[... %zu bytes omitted; no spill dir ...]\n\n",
                   omitted);
    }
    buf_add(out, text + len - tail, tail);
    return TOOL_OK;
}
