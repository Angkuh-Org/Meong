/*
 * Handler-level tests for edit: exact-unique, no card on a knowable failure,
 * replace_all, CRLF, links, stale approval, protected paths and the
 * --allow-writes path. The wire/TUI card is covered by the pty suite.
 */
#include "tools/tools.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
static const char *cwd;

static void ok(int cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        fails++;
    }
}

static void put(const char *path, const char *text, size_t len, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        perror(path);
        exit(2);
    }
    if (len && write(fd, text, len) != (ssize_t)len) {
        perror(path);
        exit(2);
    }
    close(fd);
    chmod(path, mode);
}

static int slurp(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    out[n] = '\0';
    return (int)n;
}

static int no_such(const char *path) {
    struct stat st;
    return lstat(path, &st) != 0 && errno == ENOENT;
}

static enum PermDecision auth_allow(void *ud, PermReq *req) {
    (void)ud;
    (void)req;
    return PERM_ALLOW;
}

static enum PermDecision auth_ask(void *ud, PermReq *req) {
    (void)ud;
    (void)req;
    return PERM_ASK;
}

static enum PermDecision auth_policy(void *ud, PermReq *req) {
    return perm_decide((const PermPolicy *)ud, req);
}

static int run_edit(const char *path, const char *old, const char *new,
                    int replace_all, ToolAuthorizeFn auth, void *ud, Buf *out,
                    ToolAsk *ask, char *err, size_t err_cap) {
    Buf args;
    buf_init(&args);
    buf_adds(&args, "{\"path\":");
    buf_json_str(&args, path, strlen(path));
    buf_adds(&args, ",\"old_string\":");
    buf_json_str(&args, old, strlen(old));
    buf_adds(&args, ",\"new_string\":");
    buf_json_str(&args, new, strlen(new));
    if (replace_all) {
        buf_adds(&args, ",\"replace_all\":true");
    }
    buf_addc(&args, '}');
    buf_init(out);
    perm_ask_clear(ask);
    if (err && err_cap) {
        err[0] = '\0';
    }
    int rc = tool_execute("edit", args.p, args.len, cwd, ud, auth, ask, out,
                          err, err_cap);
    buf_free(&args);
    return rc;
}

static void test_unique(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/notes.txt", dir);
    put(path, "alpha\nbeta\ngamma\n", 17, 0640);
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "beta", "BETA", 0, auth_allow, NULL, &out, &ask,
                      err, sizeof err);
    ok(rc == TOOL_OK, "unique edit returns TOOL_OK");
    char buf[64];
    ok(slurp(path, buf, sizeof buf) == 17 &&
           strcmp(buf, "alpha\nBETA\ngamma\n") == 0,
       "unique edit writes the expected bytes");
    struct stat st;
    ok(stat(path, &st) == 0 && (st.st_mode & 07777) == 0640,
       "unique edit preserves the mode");
    ok(out.p && strstr(out.p, "OK: edited ") == out.p,
       "the result starts with OK: edited");
    ok(out.p && strstr(out.p, "+1 -1") != NULL,
       "the result reports the line counts");
    ok(out.p && strstr(out.p, "2: BETA") != NULL,
       "the result includes a post-edit excerpt");
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s/.notes.txt.mng", dir);
    ok(no_such(tmp), "no temp is left after a successful edit");
    buf_free(&out);
}

static void test_overlap(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/overlap.txt", dir);
    put(path, "aaa\n", 4, 0644);
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "aa", "X", 0, auth_allow, NULL, &out, &ask, err,
                      sizeof err);
    ok(rc == TOOL_OK, "a self-overlapping needle is one match");
    char buf[16];
    ok(slurp(path, buf, sizeof buf) == 3 && strcmp(buf, "Xa\n") == 0,
       "the overlapping edit replaces only the first match");
    buf_free(&out);
}

static int has_keyed_temp(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        return 1;
    }
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, ".mng.", 5) == 0) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static void test_mng_takeover(const char *dir) {
    char path[4096];
    char canary[4096];
    snprintf(path, sizeof path, "%s/foo", dir);
    snprintf(canary, sizeof canary, "%s/.foo.mng", dir);
    put(path, "old\n", 4, 0644);
    put(canary, "USER CANARY\n", 12, 0644);
    struct timespec ts[2];
    ts[0].tv_sec = time(NULL) - 120;
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    ok(utimensat(AT_FDCWD, canary, ts, 0) == 0,
       "the user .foo.mng mtime is old");

    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "old", "new", 0, auth_allow, NULL, &out, &ask,
                      err, sizeof err);
    ok(rc == TOOL_OK, "edit succeeds with a user .foo.mng present");
    char buf[64];
    ok(slurp(path, buf, sizeof buf) == 4 && strcmp(buf, "new\n") == 0,
       "the target is edited");
    ok(slurp(canary, buf, sizeof buf) == 12 &&
           strcmp(buf, "USER CANARY\n") == 0,
       "the user .foo.mng survives");
    ok(!has_keyed_temp(dir), "no keyed temp is left after the takeover test");
    buf_free(&out);

    char name[252];
    memset(name, 'a', 250);
    name[250] = '\0';
    char longpath[4096];
    snprintf(longpath, sizeof longpath, "%s/%s", dir, name);
    put(longpath, "old\n", 4, 0644);
    rc = run_edit(longpath, "old", "new", 0, auth_allow, NULL, &out, &ask,
                  err, sizeof err);
    ok(rc == TOOL_OK, "a 250-byte basename is editable");
    ok(slurp(longpath, buf, sizeof buf) == 4 && strcmp(buf, "new\n") == 0,
       "the 250-byte basename gets the new bytes");
    ok(!has_keyed_temp(dir), "no keyed temp is left after the long name");
    buf_free(&out);
}

static void test_no_card(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/dup.txt", dir);
    put(path, "same\nkeep\nsame\n", 15, 0644);
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "same", "changed", 0, auth_ask, NULL, &out, &ask,
                      err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "matches 2 times") != NULL,
       "a duplicate is rejected with the count");
    ok(strstr(err, "lines 1, 3") != NULL,
       "a duplicate reports the competing line numbers");
    ok(ask.tool[0] == '\0', "a duplicate opens no card");
    char buf[64];
    ok(slurp(path, buf, sizeof buf) == 15 &&
           strcmp(buf, "same\nkeep\nsame\n") == 0,
       "a duplicate leaves the file unchanged");
    buf_free(&out);

    snprintf(path, sizeof path, "%s/miss.txt", dir);
    put(path, "present\n", 8, 0644);
    rc = run_edit(path, "absent", "x", 0, auth_ask, NULL, &out, &ask, err,
                  sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "not found") != NULL,
       "a missing old_string is rejected");
    ok(ask.tool[0] == '\0', "a missing old_string opens no card");
    buf_free(&out);

    rc = run_edit(path, "present", "present", 0, auth_ask, NULL, &out, &ask,
                  err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "identical") != NULL,
       "an identical replacement is rejected");
    rc = run_edit(path, "", "x", 0, auth_ask, NULL, &out, &ask, err,
                  sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "must not be empty") != NULL,
       "an empty old_string is rejected");
    buf_free(&out);
}

static void test_replace_all(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/all.txt", dir);
    put(path, "a\na\na\n", 6, 0644);
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "a", "b", 1, auth_allow, NULL, &out, &ask, err,
                      sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "x3") != NULL,
       "replace_all reports the replacement count");
    char buf[64];
    ok(slurp(path, buf, sizeof buf) == 6 &&
           strcmp(buf, "b\nb\nb\n") == 0,
       "replace_all writes every occurrence");
    buf_free(&out);
}

static void test_multiline(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/multi.txt", dir);
    put(path, "alpha\nbeta\ngamma\ndelta\n", 23, 0644);
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "beta\ngamma", "BETA\nGAMMA", 0, auth_ask, NULL,
                      &out, &ask, err, sizeof err);
    ok(rc == TOOL_NEED_PERM && ask.tool[0] != '\0',
       "a multi-line edit asks before writing");
    ok(strstr(ask.text, "-beta") && strstr(ask.text, "-gamma") &&
           strstr(ask.text, "+BETA") && strstr(ask.text, "+GAMMA"),
       "the preview shows the whole old block and the whole new block");
    ok(strstr(ask.text, " -2 +2") != NULL,
       "the preview reports two removed and two added lines");
    char buf[64];
    ok(slurp(path, buf, sizeof buf) == 23 &&
           strcmp(buf, "alpha\nbeta\ngamma\ndelta\n") == 0,
       "the ask left the multi-line file untouched");
    buf_free(&out);

    rc = run_edit(path, "beta\ngamma", "BETA\nGAMMA", 0, auth_allow, NULL,
                  &out, &ask, err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "+2 -2") != NULL,
       "the multi-line result reports +2 -2");
    ok(out.p && strstr(out.p, ":2-3") != NULL,
       "the multi-line result reports the exact 2-3 line range");
    ok(slurp(path, buf, sizeof buf) == 23 &&
           strcmp(buf, "alpha\nBETA\nGAMMA\ndelta\n") == 0,
       "the multi-line edit writes the exact new block");
    buf_free(&out);
}

static void test_truncated_preview(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/trunc.txt", dir);
    char line[201];
    memset(line, 'A', 200);
    line[200] = '\0';
    char old[810];
    size_t o = 0;
    for (int i = 0; i < 4; i++) {
        if (i) {
            old[o++] = '\n';
        }
        memcpy(old + o, line, 200);
        o += 200;
    }
    old[o] = '\0';
    char content[820];
    memcpy(content, old, o);
    content[o] = '\n';
    content[o + 1] = '\0';
    put(path, content, o + 1, 0644);

    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, old, "X", 0, auth_ask, NULL, &out, &ask, err,
                      sizeof err);
    ok(rc == TOOL_NEED_PERM && ask.text_truncated == 1,
       "an over-budget preview is marked truncated");
    ok(ask.text_len == 512, "a truncated preview reports the cap length");
    ok(strstr(ask.text, "...") != NULL,
       "a truncated preview keeps a visible marker");
    char buf[1024];
    ok(slurp(path, buf, sizeof buf) == (int)(o + 1) &&
           memcmp(buf, content, o + 1) == 0,
       "an over-budget preview writes nothing");
    buf_free(&out);
}

static void test_preview_identity(const char *dir) {
    char line[2049];
    size_t n = 0;
    unsigned x = 0x12345678u;
    for (size_t i = 0; i < 2040; i++) {
        x = x * 1103515245u + 12345u;
        line[n++] = (char)('!' + (x >> 16) % 94);
    }
    line[n++] = '\t';
    memcpy(line + n, "\xE2\x82\xAC", 3);
    n += 3;
    memcpy(line + n, "\xF0\x9F\x98\x80", 4);
    n += 4;
    line[n] = '\0';
    ok(n == 2048, "the M11 line is 2,048 bytes");

    char content[2064];
    memcpy(content, line, n);
    content[n] = '\n';
    memcpy(content + n + 1, "SENTINEL\n", 9);
    size_t clen = n + 10;
    char path[4096];
    snprintf(path, sizeof path, "%s/long.txt", dir);
    put(path, content, clen, 0644);

    Buf out;
    ToolAsk full;
    char err[512];
    int rc = run_edit(path, line, "REPLACED", 0, auth_ask, NULL, &out, &full,
                      err, sizeof err);
    ok(rc == TOOL_NEED_PERM && full.kind == TOOL_ASK_MUTATE,
       "the full-line edit asks");
    ok(strstr(full.text, "2048B -> 8B") != NULL,
       "the full-line card shows 2,048 old bytes");
    ok(strstr(full.text, "... (line truncated)") != NULL,
       "the full-line card marks the clipped line");
    buf_free(&out);

    char prefix[191];
    memcpy(prefix, line, 190);
    prefix[190] = '\0';
    ToolAsk part;
    rc = run_edit(path, prefix, "REPLACED", 0, auth_ask, NULL, &out, &part,
                  err, sizeof err);
    ok(rc == TOOL_NEED_PERM && part.kind == TOOL_ASK_MUTATE,
       "the prefix edit asks");
    ok(strstr(part.text, "190B -> 8B") != NULL,
       "the prefix card shows 190 old bytes");
    ok(strcmp(full.text, part.text) != 0,
       "the full-line and prefix cards differ");
    buf_free(&out);

    snprintf(path, sizeof path, "%s/tab.txt", dir);
    put(path, "\tindent\n", 8, 0644);
    ToolAsk tab;
    rc = run_edit(path, "\tindent", "INDENT", 0, auth_ask, NULL, &out, &tab,
                  err, sizeof err);
    ok(rc == TOOL_NEED_PERM, "the tab edit asks");
    ok(strstr(tab.text, "-    indent") != NULL,
       "a tab renders as four spaces");
    ok(strstr(tab.text, "-\tindent") == NULL, "a tab is not dropped");
    buf_free(&out);
}

static void test_delete(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/del.txt", dir);
    put(path, "alpha\nbeta\ngamma\n", 17, 0644);
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "beta\n", "", 0, auth_allow, NULL, &out, &ask,
                      err, sizeof err);
    ok(rc == TOOL_OK && out.p && strstr(out.p, "+0 -1") != NULL,
       "a deletion reports +0 -1");
    ok(out.p && strstr(out.p, "\n2:") == NULL,
       "a deletion result has no misleading post-edit excerpt");
    char buf[64];
    ok(slurp(path, buf, sizeof buf) == 12 &&
           strcmp(buf, "alpha\ngamma\n") == 0,
       "a deletion removes exactly the matched bytes");
    buf_free(&out);
}

static void test_crlf(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/crlf.txt", dir);
    put(path, "one\r\ntwo\r\n", 10, 0600);
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "two", "TWO", 0, auth_allow, NULL, &out, &ask,
                      err, sizeof err);
    char buf[64];
    ok(rc == TOOL_OK && slurp(path, buf, sizeof buf) == 10 &&
           strcmp(buf, "one\r\nTWO\r\n") == 0,
       "a CRLF file keeps its line endings");
    buf_free(&out);

    rc = run_edit(path, "TWO\n", "TWO", 0, auth_allow, NULL, &out, &ask, err,
                  sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "CRLF") != NULL,
       "an LF-only old_string on a CRLF file gets a CRLF hint");
    buf_free(&out);
}

static void test_binary(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/blob.bin", dir);
    static const char blob[] = {'a', 0, 'b'};
    put(path, blob, sizeof blob, 0644);
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "a", "z", 0, auth_ask, NULL, &out, &ask, err,
                      sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "binary") != NULL,
       "a binary file is refused before the card");
    ok(ask.tool[0] == '\0', "a binary file opens no card");
    buf_free(&out);
}

static void test_links(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/target.txt", dir);
    put(path, "x\n", 2, 0644);
    char lnk[4096];
    snprintf(lnk, sizeof lnk, "%s/link.txt", dir);
    if (symlink(path, lnk) != 0) {
        perror("symlink");
        exit(2);
    }
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(lnk, "x", "y", 0, auth_ask, NULL, &out, &ask, err,
                      sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "symlink") != NULL,
       "a final symlink is refused");
    char buf[16];
    ok(slurp(path, buf, sizeof buf) == 2 && strcmp(buf, "x\n") == 0,
       "a refused symlink leaves the target untouched");
    buf_free(&out);

    char hard[4096];
    snprintf(hard, sizeof hard, "%s/hard.txt", dir);
    if (link(path, hard) != 0) {
        perror("link");
        exit(2);
    }
    rc = run_edit(path, "x", "y", 0, auth_ask, NULL, &out, &ask, err,
                  sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "hard-linked") != NULL,
       "a hard-linked target is refused");
    unlink(hard);
    buf_free(&out);
}

static void test_stale(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/stale.txt", dir);
    put(path, "alpha\nbeta\n", 11, 0644);
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "beta", "BETA", 0, auth_ask, NULL, &out, &ask,
                      err, sizeof err);
    ok(rc == TOOL_NEED_PERM && ask.ident.valid,
       "a mutation ask carries a valid identity");
    ToolAsk saved = ask;
    buf_free(&out);

    put(path, "alpha\nbeta\ndelta\n", 17, 0644);
    PermPolicy pol = {.cwd = cwd, .call_approved = 1,
                      .approved = &saved.ident};
    rc = run_edit(path, "beta", "BETA", 0, auth_policy, &pol, &out, &ask,
                  err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "changed since approval") != NULL,
       "a file changed after the ask is refused as stale");
    char buf[64];
    ok(slurp(path, buf, sizeof buf) == 17 &&
           strcmp(buf, "alpha\nbeta\ndelta\n") == 0,
       "a stale edit writes nothing");
    buf_free(&out);
}

static void test_protected(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/.env", dir);
    put(path, "TOKEN=old\n", 10, 0644);
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "old", "new", 0, auth_allow, NULL, &out, &ask,
                      err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "protected path") != NULL,
       "a protected path is denied before the handler");
    char buf[32];
    ok(slurp(path, buf, sizeof buf) == 10 && strcmp(buf, "TOKEN=old\n") == 0,
       "a protected edit leaves the file untouched");
    buf_free(&out);
}

static void test_allow_writes(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/opt.txt", dir);
    put(path, "old\n", 4, 0644);
    PermPolicy pol = {.cwd = cwd, .perms = PERM_ALLOW_WRITES};
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(path, "old", "new", 0, auth_policy, &pol, &out, &ask,
                      err, sizeof err);
    ok(rc == TOOL_OK && ask.tool[0] == '\0',
       "--allow-writes edits without a card");
    char buf[16];
    ok(slurp(path, buf, sizeof buf) == 4 && strcmp(buf, "new\n") == 0,
       "--allow-writes still writes the right bytes");
    buf_free(&out);
}

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
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0) {
                continue;
            }
            char p[4096];
            snprintf(p, sizeof p, "%s/%s", path, de->d_name);
            rm_rf(p);
        }
        closedir(dp);
    }
    rmdir(path);
}

int main(void) {
    char root[] = "/tmp/meong-edit-XXXXXX";
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    cwd = root;
    test_unique(root);
    test_overlap(root);
    test_mng_takeover(root);
    test_no_card(root);
    test_replace_all(root);
    test_multiline(root);
    test_truncated_preview(root);
    test_preview_identity(root);
    test_delete(root);
    test_crlf(root);
    test_binary(root);
    test_links(root);
    test_stale(root);
    test_protected(root);
    test_allow_writes(root);

    if (!fails) {
        rm_rf(root);
    }
    if (fails) {
        printf("%d FAILED (tree %s)\n", fails, root);
        return 1;
    }
    printf("all edit checks passed\n");
    return 0;
}
