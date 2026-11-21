/*
 * 3A handler tests for write: create-only, explicit overwrite, identity,
 * mode/umask, links, caps and the keyed temp. The TUI card is a separate pty
 * test; this file links the handler directly so 3A stays catalog-neutral.
 */
#include "tools/tools.h"
#include "tools/write.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static int has_temp(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, ".mng.", 5) == 0) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

typedef struct {
    Buf args;
    JDoc d;
    int root;
    int dfd;
    char resolved[PATH_MAX];
    ToolPrep prep;
} Call;

static void call_init(Call *c) {
    memset(c, 0, sizeof *c);
    c->dfd = -1;
    c->prep.dfd = -1;
    buf_init(&c->args);
}

static void call_free(Call *c) {
    if (c->prep.free_priv && c->prep.priv) {
        c->prep.free_priv(c->prep.priv);
    }
    if (c->dfd >= 0) {
        close(c->dfd);
    }
    jdoc_free(&c->d);
    buf_free(&c->args);
}

static int call_prep(Call *c, const char *path, const char *content,
                     int overwrite, char *err, size_t err_cap) {
    buf_adds(&c->args, "{\"path\":");
    buf_json_str(&c->args, path, strlen(path));
    buf_adds(&c->args, ",\"content\":");
    buf_json_str(&c->args, content, strlen(content));
    if (overwrite) {
        buf_adds(&c->args, ",\"overwrite\":true");
    }
    buf_addc(&c->args, '}');
    if (jdoc_parse(&c->d, c->args.p, c->args.len) != 0) {
        snprintf(err, err_cap, "ERROR: test parse failed");
        return TOOL_ERR;
    }
    c->root = jdoc_root(&c->d);
    const char *base = NULL;
    if (perm_open_target(cwd, path, c->resolved, sizeof c->resolved,
                         &c->dfd, &base) != 0) {
        snprintf(err, err_cap, "ERROR: write: refusing path: %.200s", path);
        return TOOL_ERR;
    }
    c->prep.dfd = c->dfd;
    c->prep.base = base;
    return tool_prep_write(cwd, c->resolved, &c->d, c->root, &c->prep, err,
                           err_cap);
}

static int call_prep_raw(Call *c, const char *path, const char *args,
                         char *err, size_t err_cap) {
    buf_adds(&c->args, args);
    if (jdoc_parse(&c->d, c->args.p, c->args.len) != 0) {
        snprintf(err, err_cap, "ERROR: test parse failed");
        return TOOL_ERR;
    }
    c->root = jdoc_root(&c->d);
    const char *base = NULL;
    if (perm_open_target(cwd, path, c->resolved, sizeof c->resolved,
                         &c->dfd, &base) != 0) {
        snprintf(err, err_cap, "ERROR: write: refusing path: %.200s", path);
        return TOOL_ERR;
    }
    c->prep.dfd = c->dfd;
    c->prep.base = base;
    return tool_prep_write(cwd, c->resolved, &c->d, c->root, &c->prep, err,
                           err_cap);
}

static int call_run(Call *c, Buf *out, char *err, size_t err_cap) {
    return tool_run_write(cwd, &c->d, c->root, NULL, NULL, NULL, &c->prep,
                          out, err, err_cap);
}

static void test_create(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/new.txt", dir);
    Call c;
    call_init(&c);
    char err[512];
    Buf out;
    buf_init(&out);
    int rc = call_prep(&c, path, "created\n", 0, err, sizeof err);
    ok(rc == TOOL_OK, "create prep returns TOOL_OK");
    ok(c.prep.ident.kind == TOOL_IDENT_CREATE,
       "create prep marks the ticket CREATE");
    ok(c.prep.preview[0] != '\0' &&
           strncmp(c.prep.preview, "create ", 7) == 0,
       "create preview starts with the create verb");
    ok(strstr(c.prep.preview, "8 B, 1 line, mode 0644") != NULL,
       "create preview shows bytes, lines and mode");
    rc = call_run(&c, &out, err, sizeof err);
    ok(rc == TOOL_OK, "create run returns TOOL_OK");
    char buf[64];
    ok(slurp(path, buf, sizeof buf) == 8 &&
           strcmp(buf, "created\n") == 0,
       "create writes the exact bytes");
    struct stat st;
    ok(stat(path, &st) == 0 && (st.st_mode & 07777) == 0644,
       "create uses 0666 & ~umask (0644 at umask 022)");
    ok(st.st_nlink == 1, "create leaves one link");
    ok(out.p && strncmp(out.p, "OK: created ", 12) == 0,
       "create result verb");
    ok(has_temp(dir) == 0, "create leaves no .mng temp");
    buf_free(&out);
    call_free(&c);
}

static void test_existing_no_overwrite(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/exists.txt", dir);
    put(path, "old\n", 4, 0640);
    Call c;
    call_init(&c);
    char err[512];
    Buf out;
    buf_init(&out);
    int rc = call_prep(&c, path, "clobber\n", 0, err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "already exists") != NULL,
       "existing without overwrite is refused");
    ok(strstr(err, "overwrite:true") != NULL,
       "the refusal names the opt-in");
    ok(c.prep.ident.valid == 0, "a refused create prepares no identity");
    char buf[32];
    ok(slurp(path, buf, sizeof buf) == 4 && strcmp(buf, "old\n") == 0,
       "the refused target is unchanged");
    ok(has_temp(dir) == 0, "the refusal leaves no temp");
    buf_free(&out);
    call_free(&c);
}

static void test_overwrite(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/old.c", dir);
    put(path, "old\n", 4, 0640);
    Call c;
    call_init(&c);
    char err[512];
    Buf out;
    buf_init(&out);
    int rc = call_prep(&c, path, "new\r\n", 1, err, sizeof err);
    ok(rc == TOOL_OK, "overwrite prep returns TOOL_OK");
    ok(c.prep.ident.kind == TOOL_IDENT_EXISTING,
       "overwrite prep marks the ticket EXISTING");
    ok(strncmp(c.prep.preview, "overwrite ", 10) == 0,
       "overwrite preview starts with the overwrite verb");
    ok(strstr(c.prep.preview, "4 B -> 5 B") != NULL,
       "overwrite preview shows old -> new bytes");
    rc = call_run(&c, &out, err, sizeof err);
    ok(rc == TOOL_OK, "overwrite run returns TOOL_OK");
    char buf[64];
    ok(slurp(path, buf, sizeof buf) == 5 && strcmp(buf, "new\r\n") == 0,
       "overwrite writes the exact CRLF bytes");
    struct stat st;
    ok(stat(path, &st) == 0 && (st.st_mode & 07777) == 0640,
       "overwrite preserves the mode");
    ok(out.p && strncmp(out.p, "OK: overwrote ", 14) == 0,
       "overwrite result verb");
    ok(has_temp(dir) == 0, "overwrite leaves no .mng temp");
    buf_free(&out);
    call_free(&c);
}

static void test_identical(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/same.txt", dir);
    put(path, "same\n", 5, 0644);
    struct stat before;
    stat(path, &before);
    Call c;
    call_init(&c);
    char err[512];
    Buf out;
    buf_init(&out);
    int rc = call_prep(&c, path, "same\n", 1, err, sizeof err);
    ok(rc == TOOL_OK, "identical prep returns TOOL_OK");
    ok(strstr(c.prep.preview, "no change") != NULL,
       "identical preview says no change");
    rc = call_run(&c, &out, err, sizeof err);
    ok(rc == TOOL_OK, "identical run returns TOOL_OK");
    struct stat after;
    stat(path, &after);
    ok(after.st_ino == before.st_ino && after.st_mtim.tv_nsec ==
           before.st_mtim.tv_nsec,
       "identical run does not rename the file");
    ok(out.p && strncmp(out.p, "OK: unchanged ", 14) == 0,
       "identical result verb");
    buf_free(&out);
    call_free(&c);
}

static void test_empty(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/empty.txt", dir);
    Call c;
    call_init(&c);
    char err[512];
    Buf out;
    buf_init(&out);
    int rc = call_prep(&c, path, "", 0, err, sizeof err);
    ok(rc == TOOL_OK, "empty create prep returns TOOL_OK");
    ok(strstr(c.prep.preview, "0 B, 0 lines (empty)") != NULL,
       "empty create preview is explicit");
    rc = call_run(&c, &out, err, sizeof err);
    ok(rc == TOOL_OK, "empty create run returns TOOL_OK");
    struct stat st;
    ok(stat(path, &st) == 0 && st.st_size == 0,
       "empty create makes a zero-byte file");
    buf_free(&out);
    call_free(&c);
}

static void test_links_and_dirs(const char *dir) {
    char target[4096];
    char lnk[4096];
    snprintf(target, sizeof target, "%s/real.txt", dir);
    snprintf(lnk, sizeof lnk, "%s/link.txt", dir);
    put(target, "x\n", 2, 0644);
    if (symlink(target, lnk) != 0) {
        perror("symlink");
        exit(2);
    }
    Call c;
    call_init(&c);
    char err[512];
    Buf out;
    buf_init(&out);
    int rc = call_prep(&c, lnk, "y\n", 1, err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "refusing symlink") != NULL,
       "overwrite refuses a symlink");
    char tb[8];
    ok(slurp(target, tb, sizeof tb) >= 0,
       "the symlink target still exists");
    buf_free(&out);
    call_free(&c);

    call_init(&c);
    buf_init(&out);
    rc = call_prep(&c, dir, "x\n", 1, err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "target is a directory") != NULL,
       "overwrite refuses a directory");
    buf_free(&out);
    call_free(&c);

    char hard[4096];
    snprintf(hard, sizeof hard, "%s/hard.txt", dir);
    if (link(target, hard) != 0) {
        perror("link");
        exit(2);
    }
    call_init(&c);
    buf_init(&out);
    rc = call_prep(&c, hard, "y\n", 1, err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "hard-linked") != NULL,
       "overwrite refuses a hardlink");
    buf_free(&out);
    call_free(&c);
}

static void test_strict_overwrite(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/strict.txt", dir);
    put(path, "old\n", 4, 0644);
    char args[8192];
    snprintf(args, sizeof args,
             "{\"path\":\"%s\",\"content\":\"x\\n\","
             "\"overwrite\":\"true\"}",
             path);
    Call c;
    call_init(&c);
    char err[512];
    int rc = call_prep_raw(&c, path, args, err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "must be a boolean") != NULL,
       "a non-boolean overwrite is refused");
    call_free(&c);
}

static void test_caps(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/cap.txt", dir);
    Call c;
    call_init(&c);
    char err[512];
    Buf out;
    buf_init(&out);
    char *big = malloc((24u << 10) + 2);
    memset(big, 'A', (24u << 10) + 1);
    big[(24u << 10) + 1] = '\0';
    int rc = call_prep(&c, path, big, 0, err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "content too large") != NULL,
       "content over 24 KiB is refused");
    free(big);
    buf_free(&out);
    call_free(&c);

    call_init(&c);
    buf_init(&out);
    char slash[4096];
    snprintf(slash, sizeof slash, "%s/", dir);
    rc = call_prep(&c, slash, "x\n", 0, err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "must not end with '/'") != NULL,
       "a raw trailing slash is refused");
    buf_free(&out);
    call_free(&c);
}

static void test_races(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/race.txt", dir);
    Call c;
    call_init(&c);
    char err[512];
    Buf out;
    buf_init(&out);
    int rc = call_prep(&c, path, "mine\n", 0, err, sizeof err);
    ok(rc == TOOL_OK, "race create prep returns TOOL_OK");
    put(path, "winner\n", 7, 0644);
    rc = call_run(&c, &out, err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "appeared since approval") != NULL,
       "a leaf that appeared after prep is refused");
    char buf[32];
    ok(slurp(path, buf, sizeof buf) == 7 &&
           strcmp(buf, "winner\n") == 0,
       "the race winner's bytes survive");
    buf_free(&out);
    call_free(&c);

    snprintf(path, sizeof path, "%s/stale.txt", dir);
    put(path, "old\n", 4, 0644);
    call_init(&c);
    buf_init(&out);
    rc = call_prep(&c, path, "new\n", 1, err, sizeof err);
    ok(rc == TOOL_OK, "stale overwrite prep returns TOOL_OK");
    put(path, "changed\n", 8, 0644);
    rc = call_run(&c, &out, err, sizeof err);
    ok(rc == TOOL_ERR && strstr(err, "changed since approval") != NULL,
       "an overwrite changed after prep is refused");
    ok(slurp(path, buf, sizeof buf) == 8 &&
           strcmp(buf, "changed\n") == 0,
       "the changed target's bytes survive");
    buf_free(&out);
    call_free(&c);
}

static void test_temp_does_not_eat_user_file(const char *dir) {
    char user[4096];
    char target[4096];
    snprintf(user, sizeof user, "%s/.foo.mng", dir);
    snprintf(target, sizeof target, "%s/foo", dir);
    put(user, "keep me\n", 8, 0644);
    Call c;
    call_init(&c);
    char err[512];
    Buf out;
    buf_init(&out);
    int rc = call_prep(&c, target, "new\n", 0, err, sizeof err);
    ok(rc == TOOL_OK, "keyed-temp create prep returns TOOL_OK");
    rc = call_run(&c, &out, err, sizeof err);
    ok(rc == TOOL_OK, "keyed-temp create run returns TOOL_OK");
    char buf[16];
    ok(slurp(user, buf, sizeof buf) == 8 &&
           strcmp(buf, "keep me\n") == 0,
       "a user file named .foo.mng is not deleted");
    buf_free(&out);
    call_free(&c);
}

static void test_umask(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/private.txt", dir);
    mode_t old = umask(077);
    Call c;
    call_init(&c);
    char err[512];
    Buf out;
    buf_init(&out);
    int rc = call_prep(&c, path, "x\n", 0, err, sizeof err);
    ok(rc == TOOL_OK, "umask 077 create prep returns TOOL_OK");
    rc = call_run(&c, &out, err, sizeof err);
    ok(rc == TOOL_OK, "umask 077 create run returns TOOL_OK");
    struct stat st;
    ok(stat(path, &st) == 0 && (st.st_mode & 07777) == 0600,
       "umask 077 creates a 0600 file");
    umask(old);
    buf_free(&out);
    call_free(&c);
}

int main(void) {
    char dir[] = "/tmp/meong-write-test-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    cwd = dir;
    umask(022);
    test_create(dir);
    test_existing_no_overwrite(dir);
    test_overwrite(dir);
    test_strict_overwrite(dir);
    test_identical(dir);
    test_empty(dir);
    test_links_and_dirs(dir);
    test_caps(dir);
    test_races(dir);
    test_temp_does_not_eat_user_file(dir);
    test_umask(dir);
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) {
        return 2;
    }
    if (fails) {
        fprintf(stderr, "%d write test(s) failed\n", fails);
        return 1;
    }
    printf("write: all checks passed\n");
    return 0;
}
