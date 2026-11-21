/*
 * Locks the action-class policy: a matrix over the three base grants, the
 * per-class session grants, the one-call approval, fail-closed missing
 * resources, and the ask-record copy.
 */
#include "tools/perm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void ok(int cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        fails++;
    }
}

static void put(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs("x\n", f);
    fclose(f);
}

static enum PermDecision decide(const char *cwd, unsigned perms,
                                enum ToolAction action, const char *path,
                                const char *command, int call_approved) {
    PermReq r = {.action = action, .tool = "t", .path = path,
                 .command = command};
    PermPolicy p = {.cwd = cwd, .perms = perms,
                    .call_approved = call_approved};
    return perm_decide(&p, &r);
}

static void test_mask_matrix(const char *cwd, const char *inside,
                             const char *outside) {
    for (unsigned mask = 0; mask < 8; mask++) {
        unsigned perms = 0;
        if (mask & 1u) {
            perms |= PERM_YOLO_READ;
        }
        if (mask & 2u) {
            perms |= PERM_ALLOW_WRITES;
        }
        if (mask & 4u) {
            perms |= PERM_ALLOW_EXEC;
        }
        char label[96];

        snprintf(label, sizeof label, "mask %u: read inside allows", mask);
        ok(decide(cwd, perms, TOOL_ACT_READ, inside, NULL, 0) == PERM_ALLOW,
           label);

        snprintf(label, sizeof label, "mask %u: read outside %s", mask,
                 (mask & 1u) ? "allows" : "asks");
        ok(decide(cwd, perms, TOOL_ACT_READ, outside, NULL, 0) ==
               ((mask & 1u) ? PERM_ALLOW : PERM_ASK),
           label);

        snprintf(label, sizeof label, "mask %u: mutate inside %s", mask,
                 (mask & 2u) ? "allows" : "asks");
        ok(decide(cwd, perms, TOOL_ACT_MUTATE, inside, NULL, 0) ==
               ((mask & 2u) ? PERM_ALLOW : PERM_ASK),
           label);

        snprintf(label, sizeof label, "mask %u: mutate outside %s", mask,
                 (mask & 2u) ? "allows" : "asks");
        ok(decide(cwd, perms, TOOL_ACT_MUTATE, outside, NULL, 0) ==
               ((mask & 2u) ? PERM_ALLOW : PERM_ASK),
           label);

        snprintf(label, sizeof label, "mask %u: exec %s", mask,
                 (mask & 4u) ? "allows" : "asks");
        ok(decide(cwd, perms, TOOL_ACT_EXEC, NULL, "/bin/true", 0) ==
               ((mask & 4u) ? PERM_ALLOW : PERM_ASK),
           label);
    }
}

static void test_session_bits(const char *cwd, const char *inside,
                              const char *outside) {
    ok(decide(cwd, PERM_SESSION_READ, TOOL_ACT_READ, outside, NULL, 0) ==
           PERM_ALLOW,
       "session read allows an outside read");
    ok(decide(cwd, PERM_SESSION_READ, TOOL_ACT_MUTATE, inside, NULL, 0) ==
           PERM_ASK,
       "session read does not allow a mutation");
    ok(decide(cwd, PERM_SESSION_READ, TOOL_ACT_EXEC, NULL, "/bin/true", 0) ==
           PERM_ASK,
       "session read does not allow exec");

    ok(decide(cwd, PERM_SESSION_MUTATE, TOOL_ACT_MUTATE, outside, NULL, 0) ==
           PERM_ALLOW,
       "session mutate allows a mutation");
    ok(decide(cwd, PERM_SESSION_MUTATE, TOOL_ACT_READ, outside, NULL, 0) ==
           PERM_ASK,
       "session mutate does not allow a read");
    ok(decide(cwd, PERM_SESSION_MUTATE, TOOL_ACT_EXEC, NULL, "/bin/true", 0) ==
           PERM_ASK,
       "session mutate does not allow exec");

    ok(decide(cwd, PERM_SESSION_EXEC, TOOL_ACT_EXEC, NULL, "/bin/true", 0) ==
           PERM_ALLOW,
       "session exec allows exec");
    ok(decide(cwd, PERM_SESSION_EXEC, TOOL_ACT_READ, outside, NULL, 0) ==
           PERM_ASK,
       "session exec does not allow a read");
    ok(decide(cwd, PERM_SESSION_EXEC, TOOL_ACT_MUTATE, inside, NULL, 0) ==
           PERM_ASK,
       "session exec does not allow a mutation");
}

static void test_call_and_missing(const char *cwd, const char *outside) {
    ok(decide(cwd, 0, TOOL_ACT_READ, outside, NULL, 1) == PERM_ALLOW,
       "one-call approval allows an outside read");
    ok(decide(cwd, 0, TOOL_ACT_MUTATE, outside, NULL, 1) == PERM_DENY,
       "a mutation approval without a ticket is denied");
    ok(decide(cwd, 0, TOOL_ACT_EXEC, NULL, "/bin/true", 1) == PERM_ALLOW,
       "one-call approval allows exec");

    ToolIdentity want = {.dev = 1, .ino = 2, .size = 3, .mtime_s = 4,
                         .mtime_ns = 5, .mode = 0640, .hash = 6, .valid = 1};
    ToolIdentity got = want;
    PermReq r = {.action = TOOL_ACT_MUTATE, .tool = "edit",
                 .path = outside, .ident = &got};
    PermPolicy p = {.cwd = cwd, .call_approved = 1, .approved = &want};
    ok(perm_decide(&p, &r) == PERM_ALLOW,
       "a mutation approval with the same ticket allows");
    got.hash = 7;
    ok(perm_decide(&p, &r) == PERM_STALE,
       "a changed hash is stale, not an allow");
    got = want;
    got.ino = 99;
    ok(perm_decide(&p, &r) == PERM_STALE,
       "a different inode is stale, not an allow");
    got = want;
    p.approved = NULL;
    ok(perm_decide(&p, &r) == PERM_DENY,
       "a mutation approval without a recorded identity denies");

    ok(decide(cwd, 0, TOOL_ACT_READ, NULL, NULL, 0) == PERM_DENY,
       "read without a path denies");
    ok(decide(cwd, 0, TOOL_ACT_MUTATE, "", NULL, 0) == PERM_DENY,
       "mutate without a path denies");
    ok(decide(cwd, 0, TOOL_ACT_EXEC, NULL, NULL, 0) == PERM_DENY,
       "exec without a command denies");
}

static void test_resolve(const char *cwd, const char *inside,
                         const char *outside) {
    char out[PATH_MAX];
    ok(perm_resolve_path(cwd, "inside.txt", out, sizeof out) == 1 &&
           strcmp(out, inside) == 0,
       "relative path resolves against cwd");
    ok(perm_resolve_path(cwd, inside, out, sizeof out) == 1 &&
           strcmp(out, inside) == 0,
       "absolute path resolves to itself");
    ok(perm_resolve_path(cwd, "nope/leaf.txt", out, sizeof out) == 0 &&
           out[0] == '\0',
       "a missing parent stays unresolved");
    ok(perm_resolve_path(cwd, "~/x", out, sizeof out) == 0 && out[0] == '\0',
       "a tilde path is not expanded or treated as inside");
    char tiny[8];
    ok(perm_resolve_path(cwd, inside, tiny, sizeof tiny) == 0 &&
           tiny[0] == '\0',
       "a buffer too small refuses and clears instead of truncating");
    ok(perm_path_within_cwd(cwd, inside) == 1,
       "the resolved inside path is within cwd");
    ok(perm_path_within_cwd(cwd, outside) == 0,
       "the resolved outside path is outside cwd");
    char sibling[PATH_MAX];
    snprintf(sibling, sizeof sibling, "%sx", cwd);
    ok(perm_path_within_cwd(cwd, sibling) == 0,
       "a prefix sibling is outside cwd");
}

static void test_ask(const char *cwd, const char *outside) {
    PermReq r = {.action = TOOL_ACT_READ, .tool = "read_file", .path = outside};
    PermPolicy p = {.cwd = cwd};
    ToolAsk ask;
    memset(&ask, 0xff, sizeof ask);
    ok(perm_decide(&p, &r) == PERM_ASK, "outside read asks");
    perm_fill_ask(&ask, &r);
    ok(ask.action == TOOL_ACT_READ, "ask carries the action");
    ok(strcmp(ask.tool, "read_file") == 0, "ask carries the tool name");
    ok(ask.kind == TOOL_ASK_PATH, "a path ask has path kind");
    ok(strcmp(ask.path, outside) == 0, "ask carries the resolved path");

    PermReq m = {.action = TOOL_ACT_MUTATE, .tool = "edit", .path = outside,
                 .preview = "old\nnew\n", .preview_len = 8};
    ok(perm_decide(&p, &m) == PERM_ASK, "mutate asks");
    perm_fill_ask(&ask, &m);
    ok(ask.kind == TOOL_ASK_MUTATE, "a preview ask has mutate kind");
    ok(ask.text_len == 8 && strcmp(ask.text, "old\nnew\n") == 0,
       "ask copies the preview");

    PermReq x = {.action = TOOL_ACT_EXEC, .tool = "run",
                 .command = "/bin/true"};
    ok(perm_decide(&p, &x) == PERM_ASK, "exec asks");
    perm_fill_ask(&ask, &x);
    ok(ask.kind == TOOL_ASK_COMMAND, "a command ask has command kind");
    ok(strcmp(ask.text, "/bin/true") == 0, "ask copies the command");

    char big[TOOL_ASK_TEXT_MAX + 16];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    PermReq b = {.action = TOOL_ACT_MUTATE, .tool = "edit", .path = outside,
                 .preview = big, .preview_len = strlen(big)};
    perm_fill_ask(&ask, &b);
    ok(ask.text_truncated == 1 && strlen(ask.text) == TOOL_ASK_TEXT_MAX - 1,
       "a long preview is marked truncated");
    ok(ask.text_len == strlen(big), "the full preview length is retained");

    perm_ask_clear(&ask);
    ok(ask.tool[0] == '\0' && ask.path[0] == '\0' && ask.text[0] == '\0',
       "clearing an ask zeroes every field");
}

static void test_protected(const char *cwd, const char *outside) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/.ssh/id_rsa", cwd);
    char *slash = strrchr(p, '/');
    *slash = '\0';
    mkdir(p, 0700);
    *slash = '/';
    put(p);
    ok(perm_protected(p) == 1, "a .ssh component is protected");

    snprintf(p, sizeof p, "%s/.git/config", cwd);
    ok(perm_protected(p) == 1, "a .git component is protected");
    snprintf(p, sizeof p, "%s/.env", cwd);
    ok(perm_protected(p) == 1, ".env is protected");
    snprintf(p, sizeof p, "%s/.env.local", cwd);
    ok(perm_protected(p) == 1, "a .env.* sibling is protected");
    snprintf(p, sizeof p, "%s/.env.example", cwd);
    ok(perm_protected(p) == 0, ".env.example stays editable");
    snprintf(p, sizeof p, "%s/sub/.env", cwd);
    ok(perm_protected(p) == 1, "a nested .env is protected");
    snprintf(p, sizeof p, "%s/.github/workflows/x.yml", cwd);
    ok(perm_protected(p) == 0, ".github is not .git");
    snprintf(p, sizeof p, "%s/.gitignore", cwd);
    ok(perm_protected(p) == 0, ".gitignore is not .git");
    snprintf(p, sizeof p, "%s/authorized_keys", cwd);
    ok(perm_protected(p) == 1, "authorized_keys is protected");
    ok(perm_protected(outside) == 0, "an ordinary path is not protected");

    ToolIdentity id = {.valid = 1, .ino = 7};
    PermReq r = {.action = TOOL_ACT_MUTATE, .tool = "edit", .path = NULL,
                 .ident = &id};
    snprintf(p, sizeof p, "%s/.ssh/id_rsa", cwd);
    r.path = p;
    PermPolicy pol = {.cwd = cwd,
                      .perms = PERM_YOLO_READ | PERM_ALLOW_WRITES |
                               PERM_SESSION_MUTATE | PERM_ALLOW_EXEC |
                               PERM_SESSION_EXEC,
                      .call_approved = 1,
                      .approved = &id};
    ok(perm_decide(&pol, &r) == PERM_DENY_PROTECTED,
       "flags and an approval cannot waive a protected path");

    r.action = TOOL_ACT_READ;
    r.ident = NULL;
    ok(perm_decide(&pol, &r) == PERM_DENY_PROTECTED,
       "--yolo cannot read a protected path");
}

static void test_open_target(const char *cwd) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/odir", cwd);
    mkdir(dir, 0700);
    char file[PATH_MAX + 8];
    snprintf(file, sizeof file, "%s/file", dir);
    put(file);
    char link[PATH_MAX + 8];
    snprintf(link, sizeof link, "%s/link", dir);
    if (symlink("file", link) != 0) {
        perror("symlink");
        exit(2);
    }
    char dlink[PATH_MAX + 8];
    snprintf(dlink, sizeof dlink, "%s/dlink", cwd);
    if (symlink("odir", dlink) != 0) {
        perror("symlink");
        exit(2);
    }

    char resolved[PATH_MAX];
    const char *base = NULL;
    int dfd = -1;
    ok(perm_open_target(cwd, "odir/file", resolved, sizeof resolved, &dfd,
                        &base) == 0,
       "open_target walks a regular path");
    ok(strcmp(base, "file") == 0, "open_target returns the leaf basename");
    ok(strcmp(resolved, file) == 0,
       "open_target returns the normalised absolute path");
    int fd = openat(dfd, base, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    ok(fd >= 0, "the returned dirfd can open the leaf");
    if (fd >= 0) {
        close(fd);
    }
    close(dfd);
    dfd = -1;

    ok(perm_open_target(cwd, "odir/../odir/file", resolved,
                        sizeof resolved, &dfd, &base) == 0 &&
           strcmp(resolved, file) == 0,
       "open_target collapses .. lexically");
    close(dfd);
    dfd = -1;

    ok(perm_open_target(cwd, "odir/link", resolved, sizeof resolved, &dfd,
                        &base) == 0,
       "open_target returns a final symlink without following it");
    struct stat st;
    ok(fstatat(dfd, base, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISLNK(st.st_mode),
       "the final symlink is still a symlink for prep to refuse");
    close(dfd);
    dfd = -1;

    errno = 0;
    ok(perm_open_target(cwd, "dlink/file", resolved, sizeof resolved, &dfd,
                        &base) == -1,
       "open_target refuses a mid-path symlink");
    if (dfd >= 0) {
        close(dfd);
    }

    ok(perm_open_target(cwd, "odir/nope", resolved, sizeof resolved, &dfd,
                        &base) == 0,
       "open_target allows a missing leaf whose parent exists");
    close(dfd);
    dfd = -1;

    ok(perm_open_target(cwd, "nope/file", resolved, sizeof resolved, &dfd,
                        &base) == -1,
       "open_target refuses a missing parent");
    if (dfd >= 0) {
        close(dfd);
    }
}

int main(void) {
    char root[] = "/tmp/meong-perm-XXXXXX";
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    char inside[PATH_MAX];
    snprintf(inside, sizeof inside, "%s/inside.txt", root);
    put(inside);

    char other[] = "/tmp/meong-perm-out-XXXXXX";
    if (!mkdtemp(other)) {
        perror("mkdtemp");
        return 2;
    }
    char outside[PATH_MAX];
    snprintf(outside, sizeof outside, "%s/outside.txt", other);
    put(outside);

    test_mask_matrix(root, inside, outside);
    test_session_bits(root, inside, outside);
    test_call_and_missing(root, outside);
    test_resolve(root, inside, outside);
    test_ask(root, outside);
    test_protected(root, outside);
    test_open_target(root);

    unlink(inside);
    unlink(outside);
    rmdir(root);
    rmdir(other);
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("all perm checks passed\n");
    return 0;
}
