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
    ok(jtype(&d, root) == J_ARRAY && jlen(&d, root) == 6,
       "tools_json is an array of six tools");

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

    /* glob: pattern+path required, no limit. */
    fn = jget(&d, jgeti(&d, root, 3), "function");
    int gn = jget(&d, fn, "name");
    char *gv = gn >= 0 ? jstring_alloc(&d, gn) : NULL;
    ok(gv && strcmp(gv, "glob") == 0, "tool 3 is glob");
    free(gv);
    int gdesc = jget(&d, fn, "description");
    char *gdv = gdesc >= 0 ? jstring_alloc(&d, gdesc) : NULL;
    ok(gdv && strstr(gdv, "Files only") && strstr(gdv, "Case-sensitive") &&
           strstr(gdv, "untrusted data"),
       "glob documents files-only, case and untrusted output");
    free(gdv);
    params = jget(&d, fn, "parameters");
    props = jget(&d, params, "properties");
    req = jget(&d, params, "required");
    char *g0 = jlen(&d, req) == 2 ? jstring_alloc(&d, jgeti(&d, req, 0)) : NULL;
    char *g1 = jlen(&d, req) == 2 ? jstring_alloc(&d, jgeti(&d, req, 1)) : NULL;
    ok(g0 && g1 && strcmp(g0, "pattern") == 0 && strcmp(g1, "path") == 0,
       "glob requires pattern and path");
    free(g0);
    free(g1);
    int gp = jget(&d, props, "pattern");
    int gpp = jget(&d, props, "path");
    char *gpv = gp >= 0 ? jstring_alloc(&d, jget(&d, gp, "type")) : NULL;
    char *gppv = gpp >= 0 ? jstring_alloc(&d, jget(&d, gpp, "type")) : NULL;
    ok(gpv && gppv && strcmp(gpv, "string") == 0 &&
           strcmp(gppv, "string") == 0,
       "glob offers string pattern and path");
    free(gpv);
    free(gppv);
    int lim = jget(&d, props, "limit");
    ok(lim < 0, "glob has no limit knob");

    /* edit: path/old_string/new_string required, replace_all optional bool. */
    fn = jget(&d, jgeti(&d, root, 4), "function");
    int en = jget(&d, fn, "name");
    char *ev = en >= 0 ? jstring_alloc(&d, en) : NULL;
    ok(ev && strcmp(ev, "edit") == 0, "tool 4 is edit");
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
    fn = jget(&d, jgeti(&d, root, 5), "function");
    int wn = jget(&d, fn, "name");
    char *wv = wn >= 0 ? jstring_alloc(&d, wn) : NULL;
    ok(wv && strcmp(wv, "write") == 0, "tool 5 is write");
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
    chmod(path, 0640);

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
    ok(out.p && strstr(out.p, "mode: 0640") != NULL,
       "read_file reports the file mode");
    ok(out.p && strstr(out.p, "eol: lf") != NULL,
       "read_file reports lf line endings");
    buf_free(&out);

    chmod(path, 04755);
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", path);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "mode: 4755") != NULL,
       "read_file reports special mode bits");
    buf_free(&out);
    chmod(path, 0640);

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

    char *crlf = join(dir, "crlf.txt");
    char *mixed = join(dir, "mixed.txt");
    char *none = join(dir, "none.txt");
    char *cr = join(dir, "cr.txt");
    char *longf = join(dir, "longline.txt");
    char *pipe = join(dir, "pipe|name.txt");
    static const char crlf_text[] = "a\r\nb\r\nc\r\n";
    static const char mixed_text[] = "a\r\nb\nc\r\n";
    static const char cr_text[] = "a\rb\rc\r";
    put(crlf, crlf_text, sizeof crlf_text - 1);
    put(mixed, mixed_text, sizeof mixed_text - 1);
    put(none, "x", 1);
    put(cr, cr_text, sizeof cr_text - 1);
    Buf longbuf;
    buf_init(&longbuf);
    buf_adds(&longbuf, "one\n");
    for (int i = 0; i < 250; i++) {
        buf_addc(&longbuf, 'x');
    }
    buf_adds(&longbuf, "\ntwo\n");
    put(longf, longbuf.p, longbuf.len);
    buf_free(&longbuf);
    put(pipe, "pipe\n", 5);

    snprintf(args, sizeof args, "{\"path\":\"%s\"}", crlf);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "eol: crlf") != NULL &&
           strstr(out.p, "1: a\r\n") != NULL,
       "read_file reports crlf line endings");
    buf_free(&out);

    char *boundary = join(dir, "boundary.txt");
    Buf bbuf;
    buf_init(&bbuf);
    for (int i = 0; i < 65535; i++) {
        buf_addc(&bbuf, 'a');
    }
    buf_adds(&bbuf, "\r\nbbbbbbbbbb\r\n");
    put(boundary, bbuf.p, bbuf.len);
    buf_free(&bbuf);
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", boundary);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "eol: crlf") != NULL &&
           strstr(out.p, "eol: mixed") == NULL,
       "read_file carries CRLF across the 64 KiB chunk boundary");
    buf_free(&out);

    snprintf(args, sizeof args, "{\"path\":\"%s\"}", mixed);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "eol: mixed") != NULL,
       "read_file reports mixed line endings");
    buf_free(&out);

    snprintf(args, sizeof args, "{\"path\":\"%s\"}", none);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "eol: none") != NULL,
       "read_file reports no line ending");
    buf_free(&out);

    snprintf(args, sizeof args, "{\"path\":\"%s\"}", cr);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "lines: 1") != NULL &&
           strstr(out.p, "eol: cr") != NULL,
       "read_file reports cr-only line endings");
    buf_free(&out);

    /* >64 KiB and >400 lines: the count switches to the 16-byte bulk path
     * after the requested range, so the whole-window eol/lines fields must
     * still classify CRLF as CRLF and not "mixed". */
    char *bulk = join(dir, "bulk.txt");
    Buf kbuf;
    buf_init(&kbuf);
    for (int i = 0; i < 1600; i++) {
        for (int k = 0; k < 48; k++) {
            buf_addc(&kbuf, (char)('a' + i % 26));
        }
        buf_adds(&kbuf, "\r\n");
    }
    put(bulk, kbuf.p, kbuf.len);
    char *btail = join(dir, "bulktail.txt");
    put(btail, kbuf.p, kbuf.len - 2);
    buf_free(&kbuf);
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", bulk);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "lines: 1600") != NULL &&
           strstr(out.p, "eol: crlf") != NULL &&
           strstr(out.p, "range_bytes: 20000") != NULL,
       "read_file bulk count keeps crlf and the line total");
    buf_free(&out);

    snprintf(args, sizeof args, "{\"path\":\"%s\"}", btail);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "lines: 1600") != NULL &&
           strstr(out.p, "eol: crlf") != NULL,
       "read_file bulk counts an unterminated tail");
    buf_free(&out);

    snprintf(args, sizeof args,
             "{\"path\":\"%s\",\"start_line\":1000,\"end_line\":1005}",
             bulk);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "lines: 1600") != NULL &&
           strstr(out.p, "range_bytes: 300") != NULL,
       "read_file bulk count after a late start_line");
    buf_free(&out);

    char *latebin = join(dir, "latebin.txt");
    Buf nbuf;
    buf_init(&nbuf);
    for (int i = 0; i < 1600; i++) {
        for (int k = 0; k < 48; k++) {
            buf_addc(&nbuf, 'n');
        }
        buf_adds(&nbuf, "\r\n");
    }
    nbuf.p[70000] = '\0';
    put(latebin, nbuf.p, nbuf.len);
    buf_free(&nbuf);
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", latebin);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "eol: binary") != NULL,
       "read_file bulk count sees a NUL past the first chunk");
    buf_free(&out);

    /* CR at byte 65535 and LF at 65536: the pair straddles the chunk edge
     * where the bulk path takes over, so its first vector must inherit
     * prev_cr and still classify the whole file as CRLF. */
    char *edge = join(dir, "bulkedge.txt");
    Buf ebuf;
    buf_init(&ebuf);
    for (int i = 0; i < 1310; i++) {
        for (int k = 0; k < 48; k++) {
            buf_addc(&ebuf, 'a');
        }
        buf_adds(&ebuf, "\r\n");
    }
    for (int k = 0; k < 35; k++) {
        buf_addc(&ebuf, 'z');
    }
    buf_adds(&ebuf, "\r\n");
    for (int i = 0; i < 3000; i++) {
        for (int k = 0; k < 48; k++) {
            buf_addc(&ebuf, 'b');
        }
        buf_adds(&ebuf, "\r\n");
    }
    put(edge, ebuf.p, ebuf.len);
    buf_free(&ebuf);
    snprintf(args, sizeof args,
             "{\"path\":\"%s\",\"start_line\":1,\"end_line\":10}", edge);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "lines: 4311") != NULL &&
           strstr(out.p, "eol: crlf") != NULL &&
           strstr(out.p, "range_bytes: 500") != NULL,
       "read_file bulk keeps crlf across the chunk edge");
    buf_free(&out);

    /* LF lines then CRLF lines: the transition lands in the bulk stretch,
     * so a bulk path that adds total LF into si->lf would report "mixed"
     * as "lf" or "crlf" and miss the second style. */
    char *bmix = join(dir, "bulkmixed.txt");
    Buf mbuf;
    buf_init(&mbuf);
    for (int i = 0; i < 1600; i++) {
        for (int k = 0; k < 49; k++) {
            buf_addc(&mbuf, 'm');
        }
        buf_addc(&mbuf, '\n');
    }
    for (int i = 0; i < 1600; i++) {
        for (int k = 0; k < 48; k++) {
            buf_addc(&mbuf, 'n');
        }
        buf_adds(&mbuf, "\r\n");
    }
    put(bmix, mbuf.p, mbuf.len);
    buf_free(&mbuf);
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", bmix);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "lines: 3200") != NULL &&
           strstr(out.p, "eol: mixed") != NULL &&
           strstr(out.p, "range_bytes: 20000") != NULL,
       "read_file bulk count classifies a mixed window");
    buf_free(&out);

    snprintf(args, sizeof args,
             "{\"path\":\"%s\",\"start_line\":2,\"end_line\":2}", longf);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "range_bytes: 251") != NULL,
       "read_file reports the emitted range length");
    ok(rc == TOOL_OK && out.p && strstr(out.p, "max_line: 250") != NULL,
       "read_file reports the longest emitted line");
    buf_free(&out);

    snprintf(args, sizeof args, "{\"path\":\"%s\"}", pipe);
    rc = run("read_file", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "pipe\\|name.txt") != NULL,
       "read_file escapes a pipe in the path header");
    buf_free(&out);

    Buf wd;
    buf_init(&wd);
    buf_working_dir(&wd, "/tmp/a|b\nc]");
    ok(wd.p && strstr(wd.p, "Working directory: /tmp/a\\|b\\nc\\]") != NULL,
       "read metadata reports the working directory");
    buf_free(&wd);

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
    free(crlf);
    free(mixed);
    free(none);
    free(cr);
    free(longf);
    free(pipe);
    free(boundary);
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

    char *piped = join(dir, "pipe|entry.txt");
    put(piped, "x", 1);
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", dir);
    rc = run("list_dir", args, auth_allow, NULL, &out, &ask, err,
             sizeof err);
    ok(rc == TOOL_OK && out.p &&
           strstr(out.p, "f pipe\\|entry.txt 1") != NULL,
       "list_dir escapes a pipe in an entry name");
    buf_free(&out);

    free(piped);
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

/* ---- glob --------------------------------------------------------------- */

static int glob_run(const char *pattern, const char *path, Buf *out,
                    ToolAsk *ask, char *err, size_t err_cap) {
    char args[4096];
    snprintf(args, sizeof args, "{\"pattern\":\"%s\",\"path\":\"%s\"}",
             pattern, path);
    return run("glob", args, auth_allow, NULL, out, ask, err, err_cap);
}

static void test_glob(const char *dir) {
    char *sub = join(dir, "sub");
    char *deep = join(sub, "deep");
    char *git = join(dir, ".git");
    char *build = join(dir, "build");
    char *mods = join(dir, "node_modules");
    char *ssh = join(dir, ".ssh");
    char *ac = join(dir, "a.c");
    char *hidden = join(dir, ".hidden.c");
    char *txt = join(dir, "b.txt");
    char *subc = join(sub, "c.c");
    char *deepc = join(deep, "d.c");
    char *gitc = join(git, "config.c");
    char *buildc = join(build, "x.c");
    char *modc = join(mods, "y.c");
    char *env = join(dir, ".env");
    char *key = join(ssh, "id_rsa");
    char *linkfile = join(dir, "linkfile.c");
    char *linkdir = join(dir, "linkdir");
    Buf out;
    ToolAsk ask;
    char err[512];

    mkdir(sub, 0700);
    mkdir(deep, 0700);
    mkdir(git, 0700);
    mkdir(build, 0700);
    mkdir(mods, 0700);
    mkdir(ssh, 0700);
    put(ac, "a\n", 2);
    put(hidden, "h\n", 2);
    put(txt, "t\n", 2);
    put(subc, "c\n", 2);
    put(deepc, "d\n", 2);
    put(gitc, "g\n", 2);
    put(buildc, "b\n", 2);
    put(modc, "m\n", 2);
    put(env, "E=1\n", 4);
    put(key, "k\n", 2);
    ok(symlink("a.c", linkfile) == 0 && symlink("sub", linkdir) == 0,
       "glob fixture creates file and directory symlinks");

    int rc = glob_run("*.c", dir, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK, "glob returns TOOL_OK");
    ok(out.p && strstr(out.p, ac) && strstr(out.p, hidden) &&
           strstr(out.p, subc) && strstr(out.p, deepc),
       "glob no-slash pattern is basename at any depth");
    ok(out.p && strstr(out.p, txt) == NULL,
       "glob does not emit non-matching names");
    ok(out.p && strstr(out.p, linkfile) == NULL &&
           strstr(out.p, linkdir) == NULL,
       "glob never lists symlinks");
    ok(out.p && strstr(out.p, gitc) == NULL &&
           strstr(out.p, buildc) == NULL && strstr(out.p, modc) == NULL,
       "glob skips .git, build and node_modules directories");
    ok(out.p && strstr(out.p, env) == NULL && strstr(out.p, key) == NULL,
       "glob never emits a protected child");
    ok(out.p && strstr(out.p, "paths skipped (protected)") != NULL,
       "glob reports protected skips");
    const char *ph = out.p ? strstr(out.p, hidden) : NULL;
    const char *pa = out.p ? strstr(out.p, ac) : NULL;
    ok(ph && pa && ph < pa,
       "glob sorts output byte-wise, not by directory order");
    buf_free(&out);

    rc = glob_run("**/*.c", dir, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, ac) &&
           strstr(out.p, deepc),
       "glob **/*.c matches zero and many directories");
    buf_free(&out);

    rc = glob_run("sub/*.c", dir, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, subc) &&
           strstr(out.p, deepc) == NULL && strstr(out.p, ac) == NULL,
       "glob sub/*.c is one level only");
    buf_free(&out);

    rc = glob_run("sub/**/*.c", dir, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, subc) &&
           strstr(out.p, deepc),
       "glob sub/**/*.c recurses");
    buf_free(&out);

    rc = glob_run("./*.c", dir, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, ac) &&
           strstr(out.p, hidden) && strstr(out.p, subc) == NULL,
       "glob ./ anchors a slashless pattern at the root");
    buf_free(&out);

    rc = glob_run("*.txt", dir, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, txt) &&
           strstr(out.p, ac) == NULL,
       "glob filters by extension");
    buf_free(&out);

    rc = glob_run("A.C", dir, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "[no matches") != NULL,
       "glob is case-sensitive");
    buf_free(&out);

    rc = glob_run("{a,b}.c", dir, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p &&
           strstr(out.p, "brace expansion is not supported") != NULL,
       "glob explains an unsupported brace pattern");
    buf_free(&out);

    rc = glob_run("*.c", ac, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, ac) != NULL,
       "glob accepts a regular-file root");
    buf_free(&out);

    rc = glob_run("*.txt", ac, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "[no matches]") != NULL,
       "glob file root misses a non-matching pattern");
    buf_free(&out);

    const char *saved_cwd = test_cwd;
    test_cwd = dir;
    rc = glob_run("*.c", ".", &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "a.c") &&
           strstr(out.p, "./a.c") == NULL,
       "glob . emits root-relative paths without ./");
    buf_free(&out);
    test_cwd = saved_cwd;

    /* Result cap: 201 matches keep the lexicographically first 200. */
    char *cap = join(dir, "cap");
    mkdir(cap, 0700);
    for (int i = 0; i < 201; i++) {
        char name[32];
        snprintf(name, sizeof name, "f%03d.c", i);
        char *p = join(cap, name);
        put(p, "x\n", 2);
        free(p);
    }
    rc = glob_run("*.c", cap, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "201 matches, showing first 200"),
       "glob reports the result cap");
    ok(out.p && strstr(out.p, "f000.c") && strstr(out.p, "f200.c") == NULL,
       "glob keeps the first 200 sorted paths");
    buf_free(&out);

    /* Depth cap: the file at depth 33 is not scanned. */
    char *chain = join(dir, "chain");
    mkdir(chain, 0700);
    char *cur = xstrdup(chain);
    for (int i = 0; i < 34; i++) {
        char *next = join(cur, "d");
        mkdir(next, 0700);
        free(cur);
        cur = next;
    }
    char *bottom = join(cur, "bottom.c");
    put(bottom, "z\n", 2);
    rc = glob_run("*.c", chain, &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p &&
           strstr(out.p, "deeper directories not scanned") != NULL,
       "glob reports the depth cap");
    ok(out.p && strstr(out.p, bottom) == NULL,
       "glob does not scan past the depth cap");
    buf_free(&out);

    free(cur);
    free(bottom);
    free(chain);
    free(cap);
    free(sub);
    free(deep);
    free(git);
    free(build);
    free(mods);
    free(ssh);
    free(ac);
    free(hidden);
    free(txt);
    free(subc);
    free(deepc);
    free(gitc);
    free(buildc);
    free(modc);
    free(env);
    free(key);
    free(linkfile);
    free(linkdir);
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
    check_summary("glob",
                  "{\"pattern\":\"**/*.c\",\"path\":\"/home/u/proj/src\"}",
                  cwd, "**/*.c  src",
                  "summary shows the glob pattern unquoted and the path");
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
    char *d5 = join(root, "glob");
    mkdir(d1, 0700);
    mkdir(d2, 0700);
    mkdir(d3, 0700);
    mkdir(d4, 0700);
    mkdir(d5, 0700);

    test_tools_json();
    test_read_file(d1);
    test_list_dir(d2);
    test_search(d3, root);
    test_glob(d5);
    test_permissions(d4);
    test_bound(root);
    test_dispatch();
    test_summary();

    free(d1);
    free(d2);
    free(d3);
    free(d4);
    free(d5);
    rm_rf(root);
    printf(fails ? "FAILED: %d check(s)\n" : "ALL TOOL CHECKS PASSED\n", fails);
    return fails != 0;
}
