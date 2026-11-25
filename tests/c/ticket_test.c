/*
 * The second permission pass: the identity captured when the card opened must
 * match the file at the moment the human presses y. A same-size/same-mtime
 * rewrite and a delete between ASK and y are the two cases the handler tests
 * never exercised.
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

static int has_temp(const char *dir) {
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

static enum PermDecision auth_policy(void *ud, PermReq *req) {
    return perm_decide((const PermPolicy *)ud, req);
}

static int run_edit(const char *cwd, const char *path, const char *old,
                    const char *new, PermPolicy *p, Buf *out, ToolAsk *ask,
                    char *err, size_t cap) {
    Buf args;
    buf_init(&args);
    buf_adds(&args, "{\"path\":");
    buf_json_str(&args, path, strlen(path));
    buf_adds(&args, ",\"old_string\":");
    buf_json_str(&args, old, strlen(old));
    buf_adds(&args, ",\"new_string\":");
    buf_json_str(&args, new, strlen(new));
    buf_addc(&args, '}');
    buf_init(out);
    perm_ask_clear(ask);
    if (err && cap) {
        err[0] = '\0';
    }
    int rc = tool_execute("edit", args.p, args.len, cwd, p, auth_policy, ask,
                          out, err, cap);
    buf_free(&args);
    return rc;
}

int main(void) {
    char root[] = "/tmp/meong-ticket-XXXXXX";
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/t.txt", root);

    PermPolicy p;
    memset(&p, 0, sizeof p);
    p.cwd = root;

    Buf out;
    ToolAsk ask;
    char err[512];
    put(path, "AAAA\nKEEP\n", 10, 0644);
    int rc = run_edit(root, "t.txt", "AAAA", "BBBB", &p, &out, &ask, err,
                      sizeof err);
    ok(rc == TOOL_NEED_PERM && ask.ident.valid,
       "the first call asks and carries a valid identity");
    ToolIdentity approved = ask.ident;
    buf_free(&out);

    struct stat st;
    ok(stat(path, &st) == 0, "the pre-ask stat succeeds");
    put(path, "AAAA\nCHNG\n", 10, 0644);
    struct timespec ts[2];
    ts[0] = st.st_atim;
    ts[1] = st.st_mtim;
    ok(utimensat(AT_FDCWD, path, ts, 0) == 0,
       "the same-size rewrite restores the old mtime");

    p.call_approved = 1;
    p.approved = &approved;
    rc = run_edit(root, "t.txt", "AAAA", "BBBB", &p, &out, &ask, err,
                  sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "changed since approval") != NULL,
       "a same-size, same-mtime rewrite is stale");
    char buf[16];
    ok(slurp(path, buf, sizeof buf) == 10 &&
           strcmp(buf, "AAAA\nCHNG\n") == 0,
       "the stale call writes nothing");
    ok(has_temp(root) == 0, "the stale call leaves no temp");
    buf_free(&out);

    put(path, "AAAA\nKEEP\n", 10, 0644);
    p.call_approved = 0;
    p.approved = NULL;
    rc = run_edit(root, "t.txt", "AAAA", "BBBB", &p, &out, &ask, err,
                  sizeof err);
    ok(rc == TOOL_NEED_PERM, "the delete case asks again");
    approved = ask.ident;
    buf_free(&out);
    ok(unlink(path) == 0, "the file is deleted between ASK and y");

    p.call_approved = 1;
    p.approved = &approved;
    rc = run_edit(root, "t.txt", "AAAA", "BBBB", &p, &out, &ask, err,
                  sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "cannot stat") != NULL,
       "a delete between ASK and y is refused before the writer");
    ok(has_temp(root) == 0, "the delete case leaves no temp");
    buf_free(&out);

    if (!fails) {
        unlink(path);
        rmdir(root);
    }
    if (fails) {
        printf("%d FAILED (tree %s)\n", fails, root);
        return 1;
    }
    printf("all ticket checks passed\n");
    return 0;
}
