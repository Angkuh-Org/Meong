/* Matcher-only tests for glob.c: no Meong state, no filesystem. */
#include "tools/glob.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int fails;

static void ok(int cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        fails++;
    }
}

static void match(const char *pat, const char *path, int anydepth, int want) {
    int got = glob_match(pat, path, anydepth);
    char pcopy[PATH_MAX];
    GlobPat p;
    snprintf(pcopy, sizeof pcopy, "%s", pat);
    int compiled = glob_pat_compile(&p, pcopy) < 0
                       ? -1
                       : glob_pat_match(&p, path, anydepth);
    char label[256];
    snprintf(label, sizeof label, "match %-18s %-22s any=%d -> %d", pat, path,
             anydepth, want);
    ok(got == want && compiled == want, label);
    if (got != want || compiled != want) {
        printf("  one-shot=%d compiled=%d\n", got, compiled);
    }
}

int main(void) {
    char err[160];
    /* The 32-case prototype table (all must pass). */
    match("*.c", "src/tools/exec.c", 1, 1);
    match("*.c", "exec.c", 1, 1);
    match("src/*.c", "src/tools/exec.c", 0, 0);
    match("src/*.c", "src/exec.c", 0, 1);
    match("src/*/*.c", "src/tools/exec.c", 0, 1);
    match("**/*.c", "exec.c", 0, 1);
    match("**/*.c", "src/tools/exec.c", 0, 1);
    match("**/*.c", "a/b/c/d.c", 0, 1);
    match("src/**/*.c", "src/exec.c", 0, 1);
    match("src/**/*.c", "src/tools/exec.c", 0, 1);
    match("src/**/*.c", "src/a/b/exec.c", 0, 1);
    match("src/**", "src", 0, 0);
    match("src/**", "src/a", 0, 1);
    match("src/**", "src/a/b.c", 0, 1);
    match("**", "a", 0, 1);
    match("**", "a/b/c", 0, 1);
    match("a/**/b", "a/b", 0, 1);
    match("a/**/b", "a/x/b", 0, 1);
    match("a/**/b", "a/x/y/b", 0, 1);
    match("a/**/b", "a/x/y/c", 0, 0);
    match("?.c", "a.c", 0, 1);
    match("?.c", "ab.c", 0, 0);
    match("[a-c].c", "b.c", 0, 1);
    match("[!a].c", "b.c", 0, 1);
    match("[!a].c", "a.c", 0, 0);
    match("\\*.c", "*.c", 0, 1);
    match("\\*.c", "x.c", 0, 0);
    match("*", ".env", 0, 1);
    match(".env", ".env", 0, 1);
    match("SRC/*.C", "src/a.c", 0, 0);
    match("src/[!t]*/*.c", "src/app/main.c", 0, 1);
    match("src/[!t]*/*.c", "src/tools/exec.c", 0, 0);

    /* Anchoring: no-slash is basename-at-depth only in the default mode. */
    match("*.c", "src/tools/exec.c", 1, 1);
    match("*.c", "src/tools/exec.c", 0, 0);
    match("*.c", "exec.c", 0, 1);
    /* The handler strips a leading "./"; any=0 keeps the match root-level. */
    match("*.c", "exec.c", 0, 1);
    match("*.c", "src/exec.c", 0, 0);

    /* A star never crosses '/'; a globstar does. */
    match("src/*", "src/a/b", 0, 0);
    match("src/*", "src/a", 0, 1);
    match("src/**/b", "src/a/b", 0, 1);
    match("src/**/b", "src/a/c/b", 0, 1);

    /* Memoized DP: repeated globstars must terminate and match. */
    match("**/**/x.c", "a/b/x.c", 0, 1);
    match("**/**/**/**/**/**/**/**/x.c", "a/b/x.c", 0, 1);
    match("**/**/**/**/**/**/**/**/x.c", "a/b/y.c", 0, 0);

    /* One compiled pattern reused across varying depths: the sized memo
     * reset must not leak a previous path's result. */
    GlobPat reuse;
    char rpat[PATH_MAX];
    strcpy(rpat, "**/*.c");
    ok(glob_pat_compile(&reuse, rpat) == 2,
       "compiled pattern reports its segment count");
    ok(glob_pat_match(&reuse, "x.c", 0) == 1, "reuse: shallow match");
    ok(glob_pat_match(&reuse, "a/b/c.c", 0) == 1, "reuse: deep match");
    ok(glob_pat_match(&reuse, "a/b/c.h", 0) == 0, "reuse: deep miss");
    ok(glob_pat_match(&reuse, "a.c", 0) == 1, "reuse: shallow again");

    /* Braces are literal, not expansion. */
    match("{a,b}.c", "a.c", 1, 0);
    match("{a,b}.c", "{a,b}.c", 1, 1);

    /* Deep paths: 64 segments is the limit; 65 is invalid. */
    char deep[512];
    char *p = deep;
    for (int i = 0; i < 63; i++) {
        *p++ = 'a';
        *p++ = '/';
    }
    strcpy(p, "x.c");
    ok(glob_match("**/x.c", deep, 0) == 1, "64 path segments match **/x.c");

    char too_deep[512];
    p = too_deep;
    for (int i = 0; i < 64; i++) {
        *p++ = 'a';
        *p++ = '/';
    }
    strcpy(p, "x.c");
    ok(glob_match("**/x.c", too_deep, 0) == -1,
       "65 path segments are rejected");

    char longpat[512];
    p = longpat;
    for (int i = 0; i < 65; i++) {
        *p++ = 'a';
        *p++ = '/';
    }
    *p = '\0';
    ok(glob_match(longpat, "a.c", 0) == -1,
       "65 pattern segments are rejected");
    ok(glob_pattern_ok(longpat, err, sizeof err) == 0,
       "glob_pattern_ok rejects 65 segments");

    /* PATH_MAX guard. */
    char *huge = malloc((size_t)PATH_MAX + 16);
    if (huge) {
        memset(huge, 'a', (size_t)PATH_MAX + 15);
        huge[PATH_MAX + 15] = '\0';
        ok(glob_match("*", huge, 1) == -1, "over-long path is rejected");
        free(huge);
    }

    /* Pattern validation. */
    ok(glob_pattern_ok("*.c", err, sizeof err) == 1,
       "valid pattern is accepted");
    ok(glob_pattern_ok("", err, sizeof err) == 0,
       "empty pattern is rejected");
    ok(glob_pattern_ok("a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/"
                       "a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/"
                       "a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a",
                       err, sizeof err) == 0,
       "too many pattern segments are rejected");
    printf(fails ? "FAILS=%d\n" : "ALL PASS (%d)\n", fails);
    return fails != 0;
}
