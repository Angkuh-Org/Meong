/*
 * Exercises the read-only tool layer against a throwaway temp tree.  Every
 * expectation is an exact substring or an exact line, so the test cannot drift
 * with cwd, locale or readdir order: the handlers sort, and so do we.
 */
#include "tools/tools.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json/json.h"
#include "util/util.h"

static int fails;

static void ok(int cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        fails++;
    }
}

static void put(const char *path, const char *text, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    if (len && fwrite(text, 1, len, f) != len) {
        perror(path);
        exit(2);
    }
    fclose(f);
}

static char *join(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    char *p = xmalloc(dl + nl + 2);
    snprintf(p, dl + nl + 2, "%s/%s", dir, name);
    return p;
}

static enum PermDecision auth_allow(void *ud, PermReq *req) {
    (void)ud;
    (void)req;
    return PERM_ALLOW;
}

static enum PermDecision auth_deny(void *ud, PermReq *req) {
    (void)ud;
    (void)req;
    return PERM_DENY;
}

static enum PermDecision auth_ask(void *ud, PermReq *req) {
    (void)ud;
    (void)req;
    return PERM_ASK;
}

typedef struct {
    char last[1024];
    char tool[32];
    enum ToolAction action;
    int calls;
} Rec;

static enum PermDecision auth_rec(void *ud, PermReq *req) {
    Rec *r = ud;
    r->calls++;
    snprintf(r->last, sizeof r->last, "%s", req->path ? req->path : "");
    snprintf(r->tool, sizeof r->tool, "%s", req->tool ? req->tool : "");
    r->action = req->action;
    return PERM_ALLOW;
}

/* Allow exactly one path, ask about everything else. */
static enum PermDecision auth_ask_below(void *ud, PermReq *req) {
    const char *allowed = ud;
    return req->path && strcmp(allowed, req->path) == 0 ? PERM_ALLOW
                                                        : PERM_ASK;
}

/* Allow exactly one path, deny everything else. */
static enum PermDecision auth_deny_files(void *ud, PermReq *req) {
    const char *allowed = ud;
    return req->path && strcmp(allowed, req->path) == 0 ? PERM_ALLOW
                                                        : PERM_DENY;
}

/* The production policy, for the pre-dispatch root test. */
static enum PermDecision auth_policy(void *ud, PermReq *req) {
    return perm_decide((const PermPolicy *)ud, req);
}

static const char *test_cwd = "/";

static int run(const char *name, const char *args, ToolAuthorizeFn auth,
               void *ud, Buf *out, ToolAsk *ask, char *err,
               size_t err_cap) {
    buf_init(out);
    perm_ask_clear(ask);
    if (err && err_cap) {
        err[0] = '\0';
    }
    return tool_execute(name, args, args ? strlen(args) : 0, test_cwd, ud,
                        auth, ask, out, err, err_cap);
}

/* ---- catalog ------------------------------------------------------------ */

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p++) {
        n++;
    }
    return n;
}

static void test_tools_json(void) {
    const char *a = tools_json();
    const char *b = tools_json();
    ok(a && b && strcmp(a, b) == 0, "tools_json is byte-identical across calls");

    JDoc d = {0};
    int parsed = a && jdoc_parse(&d, a, strlen(a)) == 0;
    ok(parsed, "tools_json parses as JSON");
    if (!parsed) {
        jdoc_free(&d);
        return;
    }
    int root = jdoc_root(&d);
    ok(jtype(&d, root) == J_ARRAY && jlen(&d, root) == 5,
       "tools_json is an array of five tools");

    static const char *want[] = {"read_file", "search", "list_dir"};
    for (int i = 0; i < 3; i++) {
        int tool = jgeti(&d, root, i);
        int fn = jget(&d, tool, "function");
        int type = jget(&d, tool, "type");
        char *tv = type >= 0 ? jstring_alloc(&d, type) : NULL;
        char label[96];
        snprintf(label, sizeof label, "tool %d uses the function wrapper", i);
        ok(tv && strcmp(tv, "function") == 0, label);
        free(tv);

        int nm = jget(&d, fn, "name");
        char *nv = nm >= 0 ? jstring_alloc(&d, nm) : NULL;
        snprintf(label, sizeof label, "tool %d is %s", i, want[i]);
        ok(nv && strcmp(nv, want[i]) == 0, label);
        free(nv);

        int desc = jget(&d, fn, "description");
        char *dv = desc >= 0 ? jstring_alloc(&d, desc) : NULL;
        snprintf(label, sizeof label, "tool %d says read-only, absolute, untrusted",
                 i);
        ok(dv && strstr(dv, "Read-only") && strstr(dv, "absolute paths") &&
               strstr(dv, "untrusted data"),
           label);
        free(dv);

        int params = jget(&d, fn, "parameters");
        int props = jget(&d, params, "properties");
        int req = jget(&d, params, "required");
        snprintf(label, sizeof label, "tool %d declares an object schema", i);
        ok(jtype(&d, props) == J_OBJECT && jtype(&d, req) == J_ARRAY, label);
    }

    /* read_file: path required, start_line/end_line optional integers. */
    int fn = jget(&d, jgeti(&d, root, 0), "function");
    int params = jget(&d, fn, "parameters");
    int props = jget(&d, params, "properties");
    int req = jget(&d, params, "required");
    char *r0 = jlen(&d, req) == 1 ? jstring_alloc(&d, jgeti(&d, req, 0)) : NULL;
    ok(r0 && strcmp(r0, "path") == 0, "read_file requires only path");
    free(r0);
    int sl = jget(&d, props, "start_line");
    int el = jget(&d, props, "end_line");
    char *slv = sl >= 0 ? jstring_alloc(&d, jget(&d, sl, "type")) : NULL;
    char *elv = el >= 0 ? jstring_alloc(&d, jget(&d, el, "type")) : NULL;
    ok(slv && elv && strcmp(slv, "integer") == 0 &&
           strcmp(elv, "integer") == 0,
       "read_file offers integer start_line and end_line");
    free(slv);
    free(elv);
    int eld = el >= 0 ? jget(&d, el, "description") : -1;
    char *eldv = eld >= 0 ? jstring_alloc(&d, eld) : NULL;
    ok(eldv && strstr(eldv, "400 lines from start_line") != NULL,
       "end_line documents the default window");
    free(eldv);

    /* search: pattern+path required, glob/context_lines optional. */
    fn = jget(&d, jgeti(&d, root, 1), "function");
    params = jget(&d, fn, "parameters");
    props = jget(&d, params, "properties");
    req = jget(&d, params, "required");
    char *p0 = jlen(&d, req) == 2 ? jstring_alloc(&d, jgeti(&d, req, 0)) : NULL;
    char *p1 = jlen(&d, req) == 2 ? jstring_alloc(&d, jgeti(&d, req, 1)) : NULL;
    ok(p0 && p1 && strcmp(p0, "pattern") == 0 && strcmp(p1, "path") == 0,
       "search requires pattern and path");
    free(p0);
    free(p1);
    int gl = jget(&d, props, "glob");
    int cl = jget(&d, props, "context_lines");
    char *glv = gl >= 0 ? jstring_alloc(&d, jget(&d, gl, "type")) : NULL;
    char *clv = cl >= 0 ? jstring_alloc(&d, jget(&d, cl, "type")) : NULL;
    ok(glv && clv && strcmp(glv, "string") == 0 &&
           strcmp(clv, "integer") == 0,
       "search offers string glob and integer context_lines");
    free(glv);
    free(clv);

    /* edit: path/old_string/new_string required, replace_all optional bool. */
    fn = jget(&d, jgeti(&d, root, 3), "function");
    int en = jget(&d, fn, "name");
    char *ev = en >= 0 ? jstring_alloc(&d, en) : NULL;
    ok(ev && strcmp(ev, "edit") == 0, "tool 3 is edit");
    free(ev);
    int edesc = jget(&d, fn, "description");
    char *edv = edesc >= 0 ? jstring_alloc(&d, edesc) : NULL;
    ok(edv && strstr(edv, "byte-for-byte") && strstr(edv, "exactly once"),
       "edit documents the exact-unique contract");
    free(edv);
    params = jget(&d, fn, "parameters");
    props = jget(&d, params, "properties");
    req = jget(&d, params, "required");
    ok(jlen(&d, req) == 3, "edit requires three arguments");
    char *e0 = jlen(&d, req) == 3 ? jstring_alloc(&d, jgeti(&d, req, 0)) : NULL;
    char *e1 = jlen(&d, req) == 3 ? jstring_alloc(&d, jgeti(&d, req, 1)) : NULL;
    char *e2 = jlen(&d, req) == 3 ? jstring_alloc(&d, jgeti(&d, req, 2)) : NULL;
    ok(e0 && e1 && e2 && strcmp(e0, "path") == 0 &&
           strcmp(e1, "old_string") == 0 &&
           strcmp(e2, "new_string") == 0,
       "edit requires path, old_string and new_string");
    free(e0);
    free(e1);
    free(e2);
    int ra = jget(&d, props, "replace_all");
    char *rav = ra >= 0 ? jstring_alloc(&d, jget(&d, ra, "type")) : NULL;
    ok(rav && strcmp(rav, "boolean") == 0,
       "edit offers boolean replace_all");
    free(rav);

    /* write: path/content required, overwrite optional bool, 24 KiB cap. */
    fn = jget(&d, jgeti(&d, root, 4), "function");
    int wn = jget(&d, fn, "name");
    char *wv = wn >= 0 ? jstring_alloc(&d, wn) : NULL;
    ok(wv && strcmp(wv, "write") == 0, "tool 4 is write");
    free(wv);
    int wdesc = jget(&d, fn, "description");
    char *wdv = wdesc >= 0 ? jstring_alloc(&d, wdesc) : NULL;
    ok(wdv && strstr(wdv, "overwrite") && strstr(wdv, "24 KiB") &&
           strstr(wdv, "parent directory"),
       "write documents overwrite, the cap and the parent rule");
    free(wdv);
    params = jget(&d, fn, "parameters");
    props = jget(&d, params, "properties");
    req = jget(&d, params, "required");
    ok(jlen(&d, req) == 2, "write requires two arguments");
    char *w0 = jlen(&d, req) == 2 ? jstring_alloc(&d, jgeti(&d, req, 0)) : NULL;
    char *w1 = jlen(&d, req) == 2 ? jstring_alloc(&d, jgeti(&d, req, 1)) : NULL;
    ok(w0 && w1 && strcmp(w0, "path") == 0 &&
           strcmp(w1, "content") == 0,
       "write requires path and content");
    free(w0);
    free(w1);
    int ow = jget(&d, props, "overwrite");
    char *owv = ow >= 0 ? jstring_alloc(&d, jget(&d, ow, "type")) : NULL;
    ok(owv && strcmp(owv, "boolean") == 0,
       "write offers boolean overwrite");
    free(owv);
    jdoc_free(&d);
}

/* ---- read_file ---------------------------------------------------------- */

static void test_read_file(const char *dir) {
    static const char text[] = "alpha\nbravo\ncharlie\ndelta\necho\n";
    char *path = join(dir, "sample.txt");
    put(path, text, strlen(text));

    char args[2048];
    char want[64];
    Buf out;
    ToolAsk ask;
    char err[512];

    snprintf(args, sizeof args, "{\"path\":\"%s\"}", path);
    int rc = run("read_file", args, auth_allow, NULL, &out, &ask,                  err, sizeof err);
    ok(rc == TOOL_OK, "read_file returns TOOL_OK");
    snprintf(want, sizeof want, "bytes: %zu", strlen(text));
    ok(out.p && strstr(out.p, want) != NULL, "read_file reports byte size");
    ok(out.p && strstr(out.p, "lines: 5") != NULL, "read_file counts lines");
    ok(out.p && strstr(out.p, "mtime: 20") != NULL,
       "read_file reports an ISO-8601 UTC mtime");
    ok(out.p && strstr(out.p, "[file: ") == out.p,
       "read_file starts with the metadata header");
    ok(out.p && strstr(out.p, "1: alpha") && strstr(out.p, "5: echo"),
       "read_file shows every line by default");
    buf_free(&out);

    snprintf(args, sizeof args,
             "{\"path\":\"%s\",\"start_line\":2,\"end_line\":3}", path);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK, "read_file range returns TOOL_OK");
    ok(out.p && strstr(out.p, "2: bravo") && strstr(out.p, "3: charlie"),
       "read_file honours start_line/end_line");
    ok(out.p && !strstr(out.p, "1: alpha") && !strstr(out.p, "4: delta"),
       "read_file drops out-of-range lines");
    buf_free(&out);

    snprintf(args, sizeof args, "{\"path\":\"%s\",\"start_line\":9}", path);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "no lines in range"),
       "read_file reports an empty range");
    buf_free(&out);

    char *bin = join(dir, "blob.bin");
    static const char blob[] = {'a', 'b', 0, 'c', 'd'};
    put(bin, blob, sizeof blob);
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", bin);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "binary") != NULL,
       "read_file refuses a binary file");
    buf_free(&out);

    snprintf(args, sizeof args, "{\"path\":\"%s/nope.txt\"}", dir);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "ERROR:") == err,
       "read_file reports a missing file as ERROR:");
    buf_free(&out);

    /* A FIFO must be rejected before fopen: opening it with no writer blocks. */
    char *fifo = join(dir, "pipe");
    if (mkfifo(fifo, 0600) == 0) {
        snprintf(args, sizeof args, "{\"path\":\"%s\"}", fifo);
        rc = run("read_file", args, auth_allow, NULL, &out, &ask,                  err, sizeof err);
        ok(rc == TOOL_ERR && strstr(err, "not a regular file") != NULL,
           "read_file rejects a FIFO without blocking");
        buf_free(&out);
    }
    free(fifo);

    snprintf(args, sizeof args, "{\"path\":\"%s\",\"start_line\":0}", path);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "start_line") != NULL,
       "read_file rejects start_line < 1");
    buf_free(&out);

    /* start_line past the default window must not need an explicit end_line. */
    snprintf(args, sizeof args, "{\"path\":\"%s\",\"start_line\":4}", path);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "4: delta") &&
           strstr(out.p, "5: echo"),
       "read_file defaults end_line to start_line+399");
    buf_free(&out);

    /* null is how callers spell an unused optional; it must mean "absent". */
    snprintf(args, sizeof args,
             "{\"path\":\"%s\",\"start_line\":null,\"end_line\":null}", path);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "1: alpha") &&
           strstr(out.p, "5: echo"),
       "read_file treats null optionals as absent");
    buf_free(&out);

    /* 2,000 lines of 60 bytes: the body must stop at ~64 KiB, not dump 120 KiB. */
    char *big = join(dir, "big.txt");
    Buf textbuf;
    buf_init(&textbuf);
    for (int i = 0; i < 2000; i++) {
        buf_printf(&textbuf, "%059d\n", i);
    }
    put(big, textbuf.p, textbuf.len);
    snprintf(args, sizeof args, "{\"path\":\"%s\",\"end_line\":2000}", big);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "lines: 2000") != NULL,
       "read_file counts every line of a large file");
    ok(rc == TOOL_OK && out.len < (70u << 10),
       "read_file caps the body near 64 KiB");
    ok(out.p && strstr(out.p, "truncated at 64 KiB") != NULL,
       "read_file says when it truncated");
    buf_free(&out);
    buf_free(&textbuf);

    /* A line longer than the 64 KiB read buffer must keep one "N: " prefix. */
    char *wide = join(dir, "wide.txt");
    Buf widebuf;
    buf_init(&widebuf);
    for (int i = 0; i < 65000; i++) {
        buf_addc(&widebuf, 'a');
    }
    buf_addc(&widebuf, '\n');
    for (int i = 0; i < 1000; i++) {
        buf_addc(&widebuf, 'b');
    }
    buf_adds(&widebuf, "\ntail\n");
    put(wide, widebuf.p, widebuf.len);
    buf_free(&widebuf);
    snprintf(args, sizeof args, "{\"path\":\"%s\",\"start_line\":2}", wide);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && count_of(out.p, "2: ") == 1,
       "read_file prefixes a line that straddles a read chunk once");
    ok(out.p && strstr(out.p, "3: tail") != NULL,
       "read_file keeps the next line intact after a straddling line");
    buf_free(&out);

    free(wide);
    free(big);
    free(bin);
    free(path);
}

/* ---- list_dir ----------------------------------------------------------- */

static void test_list_dir(const char *dir) {
    char *sub = join(dir, "adir");
    char *file = join(dir, "b.txt");
    char *link = join(dir, "clink");
    mkdir(sub, 0700);
    put(file, "hello", 5);
    if (symlink("b.txt", link) != 0) {
        perror("symlink");
        exit(2);
    }

    char args[2048];
    Buf out;
    ToolAsk ask;
    char err[512];
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", dir);
    int rc = run("list_dir", args, auth_allow, NULL, &out, &ask, err,
                 sizeof err);
    ok(rc == TOOL_OK, "list_dir returns TOOL_OK");
    ok(out.p && strstr(out.p, "d adir\n") != NULL,
       "list_dir marks a directory with d");
    ok(out.p && strstr(out.p, "f b.txt 5\n") != NULL,
       "list_dir prints file size with f");
    ok(out.p && strstr(out.p, "l clink -> b.txt\n") != NULL,
       "list_dir prints a symlink target with l");

    const char *pd = out.p ? strstr(out.p, "d adir") : NULL;
    const char *pf = out.p ? strstr(out.p, "f b.txt") : NULL;
    ok(pd && pf && pd < pf, "list_dir sorts entries by name");
    buf_free(&out);

    char *empty = join(dir, "empty");
    mkdir(empty, 0700);
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", empty);
    rc = run("list_dir", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "[empty directory]") != NULL,
       "list_dir says so when the directory is empty");
    buf_free(&out);

    free(empty);
    free(file);
    free(link);
    free(sub);
}

/* ---- search ------------------------------------------------------------- */

static void test_search(const char *dir, const char *scratch) {
    char *notes = join(dir, "notes.txt");
    char *code = join(dir, "other.c");
    static const char notes_text[] = "one\ntwo\nneedle here\nfour\nfive\n";
    static const char code_text[] = "int x;\nneedle too\n";
    put(notes, notes_text, strlen(notes_text));
    put(code, code_text, strlen(code_text));

    char args[2048];
    char want[1024];
    Buf out;
    ToolAsk ask;
    char err[512];

    snprintf(args, sizeof args,
             "{\"pattern\":\"needle\",\"path\":\"%s\",\"context_lines\":1}",
             dir);
    int rc = run("search", args, auth_allow, NULL, &out, &ask, err,
                 sizeof err);
    ok(rc == TOOL_OK, "search returns TOOL_OK");
    snprintf(want, sizeof want, "%s:3: needle here", notes);
    ok(out.p && strstr(out.p, want) != NULL,
       "search prints path:line: text for a match");
    snprintf(want, sizeof want, "  %s:2: two", notes);
    ok(out.p && strstr(out.p, want) != NULL,
       "search prints preceding context with a two-space prefix");
    snprintf(want, sizeof want, "  %s:4: four", notes);
    ok(out.p && strstr(out.p, want) != NULL,
       "search prints following context");
    ok(out.p && strstr(out.p, "5: five") == NULL,
       "search stops context at context_lines");
    snprintf(want, sizeof want, "%s:2: needle too", code);
    ok(out.p && strstr(out.p, want) != NULL,
       "search walks nested entries of the root");
    buf_free(&out);

    /* A wrapping context ring must emit the two files' lines in order. */
    char *wrap = join(dir, "wrap.txt");
    static const char wrap_text[] =
        "l1\nl2\nneedle A\nl4\nneedle B\nl6\nl7\n";
    put(wrap, wrap_text, strlen(wrap_text));
    snprintf(args, sizeof args,
             "{\"pattern\":\"needle\",\"path\":\"%s\",\"glob\":\"wrap.txt\","
             "\"context_lines\":2}",
             dir);
    rc = run("search", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    snprintf(want, sizeof want,
             "  %s:1: l1\n  %s:2: l2\n%s:3: needle A\n  %s:4: l4\n"
             "%s:5: needle B\n  %s:6: l6\n  %s:7: l7\n",
             wrap, wrap, wrap, wrap, wrap, wrap, wrap);
    ok(rc == TOOL_OK && out.p && strstr(out.p, want) != NULL,
       "search wraps context in chronological order without duplicates");
    buf_free(&out);
    free(wrap);

    snprintf(args, sizeof args,
             "{\"pattern\":\"needle\",\"path\":\"%s\",\"glob\":\"*.c\"}", dir);
    rc = run("search", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "other.c:2:") != NULL &&
           strstr(out.p, "notes.txt") == NULL,
       "glob filters files by name");
    buf_free(&out);

    snprintf(args, sizeof args,
             "{\"pattern\":\"needle\",\"path\":\"%s\",\"glob\":null}", dir);
    rc = run("search", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    snprintf(want, sizeof want, "%s:3: needle here", notes);
    ok(rc == TOOL_OK && out.p && strstr(out.p, want) != NULL,
       "search treats a null glob as absent");
    buf_free(&out);

    snprintf(args, sizeof args, "{\"pattern\":\"[unclosed\",\"path\":\"%s\"}",
             dir);
    rc = run("search", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "regex") != NULL,
       "search rejects an invalid regex");
    buf_free(&out);

    snprintf(args, sizeof args, "{\"pattern\":\"zzz\",\"path\":\"%s\"}", dir);
    rc = run("search", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "[no matches]") != NULL,
       "search reports no matches");
    buf_free(&out);

    /* Skip-listed directories and symlinked directories are never entered. */
    static const char *skip[] = {".git", "build", "node_modules"};
    for (unsigned i = 0; i < sizeof skip / sizeof skip[0]; i++) {
        char *d = join(dir, skip[i]);
        mkdir(d, 0700);
        char *f = join(d, "hidden.txt");
        put(f, "needle hidden\n", 14);
        free(f);
        free(d);
    }
    char *outside = join(scratch, "outside");
    char *linkdir = join(dir, "linkdir");
    mkdir(outside, 0700);
    char *of = join(outside, "leaked.txt");
    put(of, "needle leaked\n", 14);
    if (symlink(outside, linkdir) != 0) {
        perror("symlink");
        exit(2);
    }
    snprintf(args, sizeof args, "{\"pattern\":\"needle\",\"path\":\"%s\"}",
             dir);
    rc = run("search", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "hidden.txt") == NULL,
       "search skips .git, build and node_modules");
    ok(out.p && strstr(out.p, "leaked.txt") == NULL,
       "search does not follow a symlinked directory");
    buf_free(&out);

    /* A deny (not an ask) skips the file but must not hide the skip. */
    rc = run("search", args, auth_deny_files, (void *)dir, &out, &ask,
             err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "files skipped (permission)"),
       "search reports files it skipped by permission");
    buf_free(&out);
    free(of);
    free(linkdir);
    free(outside);

    /* Pointing at a file searches just that file. */
    snprintf(args, sizeof args, "{\"pattern\":\"needle\",\"path\":\"%s\"}",
             notes);
    rc = run("search", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    snprintf(want, sizeof want, "%s:3: needle here", notes);
    ok(rc == TOOL_OK && out.p && strstr(out.p, want) != NULL &&
           strstr(out.p, "other.c") == NULL,
       "search on a file searches just that file");
    buf_free(&out);

    /* Match cap: 250 hits print 200 and say so. */
    char *many = join(dir, "many.txt");
    Buf mb;
    buf_init(&mb);
    for (int i = 0; i < 250; i++) {
        buf_adds(&mb, "hit\n");
    }
    put(many, mb.p, mb.len);
    buf_free(&mb);
    snprintf(args, sizeof args, "{\"pattern\":\"hit\",\"path\":\"%s\"}", many);
    rc = run("search", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && count_of(out.p, ": hit\n") == 200,
       "search caps matches at 200");
    ok(out.p && strstr(out.p, "[... stopped: 200 matches") != NULL,
       "search reports the match cap");
    buf_free(&out);
    free(many);

    free(code);
    free(notes);
}

/* ---- permissions -------------------------------------------------------- */

static void test_permissions(const char *dir) {
    char *path = join(dir, "secret.txt");
    put(path, "x\n", 2);

    char args[2048];
    Buf out;
    ToolAsk ask;
    char err[512];
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", path);

    int rc = run("read_file", args, auth_deny, NULL, &out, &ask, err,
                 sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "permission denied") != NULL &&
           strstr(err, path) != NULL,
       "denied read_file returns TOOL_ERR naming the path");
    buf_free(&out);

    rc = run("read_file", args, auth_ask, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_NEED_PERM, "asked read_file returns TOOL_NEED_PERM");
    ok(strcmp(ask.path, path) == 0, "asked read_file reports the exact path");
    ok(out.len == 0, "asked read_file executes nothing");
    buf_free(&out);

    rc = run("list_dir", args, auth_ask, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_NEED_PERM && strcmp(ask.path, path) == 0,
       "asked list_dir reports the exact path");
    buf_free(&out);

    /* The authorize callback must see the path that is about to be opened. */
    Rec rec;
    memset(&rec, 0, sizeof rec);
    rc = run("read_file", args, auth_rec, &rec, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && rec.calls >= 1 && strcmp(rec.last, path) == 0,
       "authorize receives the opened path");
    ok(strcmp(rec.tool, "read_file") == 0 && rec.action == TOOL_ACT_READ,
       "authorize receives the tool name and read class");
    buf_free(&out);

    /* The kill switch denies through the real dispatcher, before a card. */
    char *blocked = join(dir, "blocked.txt");
    char wargs[2048];
    snprintf(wargs, sizeof wargs,
             "{\"path\":\"%s\",\"content\":\"x\\n\"}", blocked);
    PermPolicy nopol = {.cwd = dir,
                        .perms = PERM_NO_MUTATE | PERM_ALLOW_WRITES};
    rc = run("write", wargs, auth_policy, &nopol, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_ERR &&
           strstr(err, "ERROR: mutation is disabled (MEONG_NO_MUTATE)") !=
               NULL &&
           strstr(err, "do not retry: write") != NULL,
       "no-mutate denies write with the switch-specific error");
    ok(access(blocked, F_OK) != 0,
       "no-mutate write creates no target");
    ok(ask.tool[0] == '\0',
       "no-mutate writes no permission card");
    buf_free(&out);
    free(blocked);

    /* A search that needs approval anywhere returns no partial result. */
    char *nested = join(dir, "nested");
    mkdir(nested, 0700);
    char *inner = join(nested, "inner.txt");
    put(inner, "needle\n", 7);
    char dargs[2048];
    snprintf(dargs, sizeof dargs, "{\"pattern\":\"needle\",\"path\":\"%s\"}",
             dir);
    rc = run("search", dargs, auth_ask_below, (void *)dir, &out, &ask,
             err, sizeof err);
    ok(rc == TOOL_NEED_PERM && strcmp(ask.path, inner) == 0,
       "search asks about a file before reading it");
    ok(out.len == 0, "search returns no partial output on TOOL_NEED_PERM");
    buf_free(&out);

    /* The production policy asks on the root the card shows, before walking. */
    char other[] = "/tmp/meong-tools-out-XXXXXX";
    if (mkdtemp(other)) {
        char *of = join(other, "outside.txt");
        put(of, "needle\n", 7);
        char oargs[2048];
        snprintf(oargs, sizeof oargs,
                 "{\"pattern\":\"needle\",\"path\":\"%s\"}", other);
        PermPolicy pol = {.cwd = dir};
        rc = run("search", oargs, auth_policy, &pol, &out, &ask, err,
                 sizeof err);
        ok(rc == TOOL_NEED_PERM && strcmp(ask.path, other) == 0,
           "search asks on its root before walking");
        ok(out.len == 0, "root ask returns no partial output");
        buf_free(&out);
        free(of);
        rmdir(other);
    }

    free(inner);
    free(nested);
    free(path);
}

/* ---- bounding ----------------------------------------------------------- */

static void test_bound(const char *dir) {
    char *spill = join(dir, "spill");
    Buf out;
    char err[256];

    static const char small[] = "short result\n";
    buf_init(&out);
    ok(tool_bound_result("call-1", small, strlen(small), spill, &out, err,
                         sizeof err) == TOOL_OK,
       "bound: small text returns TOOL_OK");
    ok(out.len == strlen(small) && memcmp(out.p, small, out.len) == 0 &&
           strstr(out.p, "omitted") == NULL,
       "bound: small text passes through unchanged");
    buf_free(&out);

    size_t n = TOOL_INLINE_MAX * 3 + 7;
    char *big = xmalloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        big[i] = (char)('a' + (int)(i % 26));
    }
    big[n] = '\0';

    buf_init(&out);
    ok(tool_bound_result("call/2:big", big, n, spill, &out, err, sizeof err) ==
           TOOL_OK,
       "bound: large text returns TOOL_OK");
    ok(out.len < n, "bound: large text is bounded");
    ok(out.p && strstr(out.p, "bytes omitted") != NULL,
       "bound: large text carries the omission notice");
    ok(out.p && strstr(out.p, "full output:") != NULL,
       "bound: large text points at the spill file");
    ok(out.p && strstr(out.p, "call2big.txt") != NULL,
       "bound: the call id is sanitized into the spill name");
    size_t head = TOOL_INLINE_MAX / 5;
    size_t tail = TOOL_INLINE_MAX - head;
    ok(out.p && memcmp(out.p, big, head) == 0, "bound: head is the first 1/5");
    ok(out.p && out.len > tail && memcmp(out.p + out.len - tail, big + n - tail,
                                         tail) == 0,
       "bound: tail is the last 4/5");

    char *path = join(spill, "call2big.txt");
    size_t got = 0;
    char *text = read_file(path, &got);
    ok(text && got == n && memcmp(text, big, n) == 0,
       "bound: the spill file holds the full text");
    free(text);
    buf_free(&out);

    buf_init(&out);
    ok(tool_bound_result("x", big, n, "/proc/meong-nope", &out, err,
                         sizeof err) == TOOL_OK,
       "bound: a failed spill is not fatal");
    ok(out.p && strstr(out.p, "spill to") != NULL,
       "bound: a failed spill is reported inline");
    ok(out.p && strstr(out.p, "bytes omitted") != NULL,
       "bound: the failed spill still states what was omitted");
    buf_free(&out);

    buf_init(&out);
    ok(tool_bound_result("x", big, n, NULL, &out, err, sizeof err) == TOOL_OK &&
           out.p && strstr(out.p, "no spill dir") != NULL,
       "bound: no spill dir degrades to a notice");
    buf_free(&out);

    free(path);
    free(big);
    free(spill);
}

/* ---- dispatch ----------------------------------------------------------- */

static void test_dispatch(void) {
    Buf out;
    ToolAsk ask;
    char err[512];

    int rc = run("nope", "{}", auth_allow, NULL, &out, &ask, err,
                 sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "unknown tool") != NULL,
       "unknown tool returns TOOL_ERR");
    buf_free(&out);

    rc = run("read_file", "[]", auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "JSON object") != NULL,
       "non-object arguments are rejected");
    buf_free(&out);

    rc = run("read_file", "{oops", auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "not valid JSON") != NULL,
       "malformed arguments are rejected");
    buf_free(&out);

    rc = run("read_file", "{}", auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "missing required argument: path") != NULL,
       "missing path is named in the error");
    buf_free(&out);

    rc = run("read_file", "{\"path\":7}", auth_allow, NULL, &out, &ask,
             err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "must be a string") != NULL,
       "wrong argument type is named in the error");
    buf_free(&out);

    ToolCall c = {.id = xstrdup("id"), .name = xstrdup("read_file"),
                  .args = xstrdup("{}"), .args_len = 2};
    tool_call_free(&c);
    ok(!c.id && !c.name && !c.args && c.args_len == 0,
       "tool_call_free nulls every field");
    tool_call_free(NULL);
    ok(1, "tool_call_free tolerates NULL");
}

/* ---- main --------------------------------------------------------------- */

static void rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    DIR *dp = opendir(path);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                continue;
            }
            char *p = join(path, de->d_name);
            rm_rf(p);
            free(p);
        }
        closedir(dp);
    }
    rmdir(path);
}

static void check_summary(const char *name, const char *args,
                          const char *cwd, const char *want,
                          const char *what) {
    char out[256];
    tool_call_summary(name, args, strlen(args), cwd, out, sizeof out);
    if (strcmp(out, want) != 0) {
        printf("  summary got %s, want %s\n", out, want);
    }
    ok(strcmp(out, want) == 0, what);
}

static void test_summary(void) {
    const char *cwd = "/home/u/proj";
    check_summary("read_file",
                  "{\"path\":\"/home/u/proj/src/a.c\",\"start_line\":10,"
                  "\"end_line\":20}",
                  cwd, "src/a.c:10-20",
                  "summary shortens the cwd and range");
    check_summary("read_file",
                  "{\"path\":\"/home/u/proj/src/a.c\",\"start_line\":10}",
                  cwd, "src/a.c:10-",
                  "summary keeps an open-ended range");
    check_summary("read_file",
                  "{\"path\":\"/home/u/proj/src/a.c\",\"end_line\":20}", cwd,
                  "src/a.c:1-20", "summary defaults a missing start to line 1");
    check_summary("search",
                  "{\"pattern\":\"tool_\",\"path\":\"/home/u/proj/src\","
                  "\"glob\":\"*.c\"}",
                  cwd, "\"tool_\"  src  *.c",
                  "summary flattens the search pattern and glob");
    check_summary("list_dir", "{\"path\":\"/home/u/proj\"}", cwd, ".",
                  "summary prints the cwd as a dot");
    check_summary("edit",
                  "{\"path\":\"/home/u/proj/src/a.c\",\"old_string\":\"x\","
                  "\"new_string\":\"y\"}",
                  cwd, "src/a.c",
                  "summary shows only the edit path, never old_string");
    check_summary("write",
                  "{\"path\":\"/home/u/proj/src/a.c\",\"content\":\"x\"}",
                  cwd, "+ src/a.c",
                  "summary marks a create-only write with +");
    check_summary("write",
                  "{\"path\":\"/home/u/proj/src/a.c\",\"content\":\"x\","
                  "\"overwrite\":true}",
                  cwd, "~ src/a.c",
                  "summary marks an overwrite-permitted write with ~");
    const char *home = getenv("HOME");
    if (home && *home) {
        char args[512];
        char want[512];
        snprintf(args, sizeof args, "{\"path\":\"%s/notes.txt\"}", home);
        snprintf(want, sizeof want, "~/notes.txt");
        check_summary("read_file", args, cwd, want,
                      "summary shortens the home directory");
    }
    check_summary("nope", "{\"a\": 1,\n \"b\":2}", cwd, "{\"a\": 1, \"b\":2}",
                  "summary compacts arguments for an unknown tool");
    check_summary("read_file", "not json", cwd, "not json",
                  "summary falls back to raw arguments on bad JSON");
}

int main(void) {
    char root[] = "/tmp/meong-tools-XXXXXX";
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    test_cwd = root;
    char *d1 = join(root, "read");
    char *d2 = join(root, "list");
    char *d3 = join(root, "search");
    char *d4 = join(root, "perm");
    mkdir(d1, 0700);
    mkdir(d2, 0700);
    mkdir(d3, 0700);
    mkdir(d4, 0700);

    test_tools_json();
    test_read_file(d1);
    test_list_dir(d2);
    test_search(d3, root);
    test_permissions(d4);
    test_bound(root);
    test_dispatch();
    test_summary();

    free(d1);
    free(d2);
    free(d3);
    free(d4);
    rm_rf(root);
    printf(fails ? "FAILED: %d check(s)\n" : "ALL TOOL CHECKS PASSED\n", fails);
    return fails != 0;
}
