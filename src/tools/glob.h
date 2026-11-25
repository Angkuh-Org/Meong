#ifndef MEONG_TOOLS_GLOB_H
#define MEONG_TOOLS_GLOB_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define GLOB_SEG_MAX 64

/*
 * A compiled pattern. seg[] points into the caller's mutable copy, split in
 * place once before a walk. path/memo are per-match scratch, reused across
 * files so a per-file match never reserves a PATH_MAX copy or a 65x65 memo on
 * the stack. One match at a time per object: match_from uses memo recursively.
 */
typedef struct {
    char *seg[GLOB_SEG_MAX];
    int n;
    int has_slash;
    int globstar;
    char path[PATH_MAX];
    int8_t memo[GLOB_SEG_MAX + 1][GLOB_SEG_MAX + 1];
} GlobPat;

/*
 * Split a mutable pattern copy into segments and detect '/' and '**'.
 * Returns the segment count, or -1 when the pattern exceeds GLOB_SEG_MAX.
 */
int glob_pat_compile(GlobPat *p, char *pattern);

/*
 * Match one relative path against a compiled pattern. basename_any_depth has
 * the same meaning as in glob_match(). Returns 1, 0, or -1 on an invalid path.
 */
int glob_pat_match(GlobPat *p, const char *path, int basename_any_depth);

/*
 * One-shot match. Compiles a private copy of the pattern; use this when the
 * pattern is not reused across files.
 *
 * basename_any_depth: when true, a pattern with no '/' matches the basename
 * at any depth (e.g. "*.c" matches "src/tools/exec.c"). When false, a
 * pattern with no '/' matches only a root-level path (the "./" form).
 *
 * Returns 1 on a match, 0 on no match, -1 on an invalid pattern/path (too
 * long or more than GLOB_SEG_MAX segments).
 */
int glob_match(const char *pattern, const char *path, int basename_any_depth);

/*
 * Validate a normalized pattern before a walk. On failure writes a
 * model-facing "ERROR: ..." message and returns 0.
 */
int glob_pattern_ok(const char *pattern, char *err, size_t err_cap);

#endif
