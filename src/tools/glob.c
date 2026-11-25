/*
 * Filename glob matcher.  One segment at a time through fnmatch(3), plus a
 * whole-segment ** walk.  The flag argument is always 0 and never a hardcoded
 * FNM_* value: glibc and bionic assign different numeric values to
 * FNM_PATHNAME/FNM_NOESCAPE, and fnmatch(..., 0) lets '*' cross '/', so a
 * whole path must never be passed to one call.
 *
 * A walk compiles the pattern once (glob_pat_compile) and reuses its scratch
 * across files: the old per-file glob_match() copied both strings into two
 * PATH_MAX buffers and cleared the whole 65x65 memo before every file.
 */
#include "tools/glob.h"

#include <fnmatch.h>
#include <stdio.h>
#include <string.h>

/* Split in place; repeated and trailing slashes collapse. */
static int split_seg(char *s, char **seg, int max) {
    int n = 0;
    while (*s) {
        while (*s == '/') {
            *s++ = '\0';
        }
        if (!*s) {
            break;
        }
        if (n == max) {
            return -1;
        }
        seg[n++] = s;
        while (*s && *s != '/') {
            s++;
        }
    }
    return n;
}

static int is_globstar(const char *s) {
    return s[0] == '*' && s[1] == '*' && s[2] == '\0';
}

int glob_pat_compile(GlobPat *p, char *pattern) {
    p->has_slash = strchr(pattern, '/') != NULL;
    p->n = split_seg(pattern, p->seg, GLOB_SEG_MAX);
    p->globstar = 0;
    if (p->n < 0) {
        return -1;
    }
    for (int i = 0; i < p->n; i++) {
        if (is_globstar(p->seg[i])) {
            p->globstar = 1;
        }
    }
    return p->n;
}

static int match_from(char **p, int pn, int pi, char **s, int sn, int si,
                      int8_t memo[GLOB_SEG_MAX + 1][GLOB_SEG_MAX + 1]) {
    if (memo[pi][si] >= 0) {
        return memo[pi][si];
    }
    int r;
    if (pi == pn) {
        r = si == sn;
    } else if (is_globstar(p[pi])) {
        if (pi == pn - 1) {
            /* A trailing globstar needs at least one entry; the directory
             * itself does not match. */
            r = si < sn;
        } else {
            r = 0;
            for (int k = si; k <= sn; k++) {
                if (match_from(p, pn, pi + 1, s, sn, k, memo)) {
                    r = 1;
                    break;
                }
            }
        }
    } else if (si == sn) {
        r = 0;
    } else if (fnmatch(p[pi], s[si], 0) != 0) {
        r = 0;
    } else {
        r = match_from(p, pn, pi + 1, s, sn, si + 1, memo);
    }
    memo[pi][si] = (int8_t)r;
    return r;
}

int glob_pat_match(GlobPat *p, const char *path, int basename_any_depth) {
    size_t sl = strlen(path);
    if (sl >= PATH_MAX) {
        return -1;
    }
    if (basename_any_depth && !p->has_slash && p->n == 1) {
        const char *b = strrchr(path, '/');
        b = b ? b + 1 : path;
        return fnmatch(p->seg[0], b, 0) == 0;
    }
    char *s[GLOB_SEG_MAX];
    memcpy(p->path, path, sl + 1);
    int sn = split_seg(p->path, s, GLOB_SEG_MAX);
    if (sn < 0) {
        return -1;
    }
    if (p->n == 0) {
        return 0;
    }
    if (!p->globstar) {
        /* No **: the match is linear, so skip the memo and the recursion. */
        if (p->n != sn) {
            return 0;
        }
        /* TODO(perf): fnmatch is 54-117 ns and ~9.5% of walk Ir (callgrind,
         * 1k files). A matcher for plain '*'/'?' segments could beat it, but
         * '[' classes, '\' escapes and non-ASCII must fall back to fnmatch;
         * not worth it while the walk stays syscall-bound. Covers the
         * match_from() call site too. */
        for (int i = 0; i < sn; i++) {
            if (fnmatch(p->seg[i], s[i], 0) != 0) {
                return 0;
            }
        }
        return 1;
    }
    /* Only the cells this path can read need clearing; si never exceeds sn. */
    for (int i = 0; i <= p->n; i++) {
        memset(p->memo[i], -1, (size_t)(sn + 1));
    }
    return match_from(p->seg, p->n, 0, s, sn, 0, p->memo);
}

int glob_match(const char *pattern, const char *path, int basename_any_depth) {
    if (!pattern || !path) {
        return -1;
    }
    size_t pl = strlen(pattern);
    size_t sl = strlen(path);
    if (pl == 0 || pl >= PATH_MAX || sl >= PATH_MAX) {
        return -1;
    }
    if (basename_any_depth && !memchr(pattern, '/', pl)) {
        const char *b = strrchr(path, '/');
        b = b ? b + 1 : path;
        return fnmatch(pattern, b, 0) == 0;
    }
    GlobPat p;
    char pb[PATH_MAX];
    memcpy(pb, pattern, pl + 1);
    if (glob_pat_compile(&p, pb) < 0) {
        return -1;
    }
    return glob_pat_match(&p, path, basename_any_depth);
}

int glob_pattern_ok(const char *pattern, char *err, size_t err_cap) {
    if (!pattern || !*pattern) {
        if (err && err_cap) {
            snprintf(err, err_cap, "ERROR: argument pattern must not be empty");
        }
        return 0;
    }
    size_t pl = strlen(pattern);
    if (pl >= PATH_MAX) {
        if (err && err_cap) {
            snprintf(err, err_cap, "ERROR: glob pattern is too long");
        }
        return 0;
    }
    int n = 0;
    const char *p = pattern;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        n++;
        while (*p && *p != '/') {
            p++;
        }
    }
    if (n == 0) {
        if (err && err_cap) {
            snprintf(err, err_cap,
                     "ERROR: glob pattern must contain a name");
        }
        return 0;
    }
    if (n > GLOB_SEG_MAX) {
        if (err && err_cap) {
            snprintf(err, err_cap,
                     "ERROR: glob pattern has too many path segments (max %d)",
                     GLOB_SEG_MAX);
        }
        return 0;
    }
    return 1;
}
