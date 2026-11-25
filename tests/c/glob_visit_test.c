/* Visit-cap test: compile exec.c with -DGLOB_VISIT_MAX=4. */
#include "tools/tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void ok(int cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        fails++;
    }
}

static enum PermDecision auth_allow(void *ud, PermReq *req) {
    (void)ud;
    (void)req;
    return PERM_ALLOW;
}

int main(void) {
    char tmpl[] = "/tmp/p5visitXXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        perror("mkdtemp");
        return 2;
    }

    char *paths[10];
    for (int i = 0; i < 10; i++) {
        char name[32];
        snprintf(name, sizeof name, "f%02d.c", i);
        size_t n = strlen(dir) + strlen(name) + 2;
        char *p = malloc(n);
        if (!p) {
            return 2;
        }
        snprintf(p, n, "%s/%s", dir, name);
        FILE *f = fopen(p, "wb");
        if (!f) {
            perror(p);
            return 2;
        }
        fputs("x\n", f);
        fclose(f);
        paths[i] = p;
    }

    char args[1024];
    snprintf(args, sizeof args, "{\"pattern\":\"*.c\",\"path\":\"%s\"}", dir);
    Buf out;
    buf_init(&out);
    ToolAsk ask;
    perm_ask_clear(&ask);
    char err[512];
    int rc = tool_execute("glob", args, strlen(args), "/", NULL, auth_allow,
                          &ask, &out, err, sizeof err);
    ok(rc == TOOL_OK, "visit-cap glob returns TOOL_OK");
    ok(out.p && strstr(out.p, "incomplete: visited 4 entries") != NULL,
       "visit cap prints the incomplete marker");

    int emitted = 0;
    if (out.p) {
        for (const char *p = out.p; *p;) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len > 2 && memcmp(p + len - 2, ".c", 2) == 0) {
                emitted++;
            }
            if (!nl) {
                break;
            }
            p = nl + 1;
        }
    }
    ok(emitted > 0 && emitted <= 4,
       "visit cap emits no more than the entries it read");

    buf_free(&out);
    for (int i = 0; i < 10; i++) {
        unlink(paths[i]);
        free(paths[i]);
    }
    rmdir(dir);
    printf(fails ? "FAILED: %d check(s)\n" : "ALL VISIT CHECKS PASSED\n",
           fails);
    return fails != 0;
}
