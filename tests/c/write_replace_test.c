/*
 * Locks the Phase 2 write primitive: atomic replace, mode preservation,
 * symlink/hardlink refusal, temp collision, and the approval hash.
 */
#include "util/util.h"

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

static void test_replace(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/target.txt", dir);
    put(path, "old\n", 4, 0640);

    ok(write_file_replace(path, "new\n", 4) == 0,
       "write_file_replace replaces an existing file");
    char buf[64];
    ok(slurp(path, buf, sizeof buf) == 4 && strcmp(buf, "new\n") == 0,
       "the new bytes are on disk");
    struct stat st;
    ok(stat(path, &st) == 0 && (st.st_mode & 07777) == 0640,
       "the original mode survives the rename");
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s/.target.txt.mng", dir);
    ok(no_such(tmp), "no temp is left after a success");

    char lnk[4096];
    snprintf(lnk, sizeof lnk, "%s/link.txt", dir);
    if (symlink(path, lnk) != 0) {
        perror("symlink");
        exit(2);
    }
    errno = 0;
    ok(write_file_replace(lnk, "x", 1) == -1 && errno == EINVAL,
       "a final symlink is refused");
    ok(slurp(path, buf, sizeof buf) == 4 && strcmp(buf, "new\n") == 0,
       "a refused symlink leaves the target untouched");

    char hard[4096];
    snprintf(hard, sizeof hard, "%s/hard.txt", dir);
    if (link(path, hard) != 0) {
        perror("link");
        exit(2);
    }
    errno = 0;
    ok(write_file_replace(path, "y", 1) == -1 && errno == EMLINK,
       "a hard-linked target is refused");
    unlink(hard);

    char busy[4096];
    snprintf(busy, sizeof busy, "%s/.target.txt.mng", dir);
    put(busy, "", 0, 0600);
    errno = 0;
    ok(write_file_replace(path, "z", 1) == -1 && errno == EEXIST,
       "a live temp is not stolen");
    ok(slurp(path, buf, sizeof buf) == 4 && strcmp(buf, "new\n") == 0,
       "a live temp leaves the target untouched");
    unlink(busy);

    char mode[4096];
    snprintf(mode, sizeof mode, "%s/mode.txt", dir);
    put(mode, "a\n", 2, 0755);
    ok(write_file_replace(mode, "b\n", 2) == 0 && stat(mode, &st) == 0 &&
           (st.st_mode & 07777) == 0755,
       "0755 is preserved");
}

static void test_at(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/at.txt", dir);
    put(path, "old\n", 4, 0600);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ok(dfd >= 0, "open the parent for the _at form");
    if (dfd < 0) {
        return;
    }
    struct stat st;
    ok(fstatat(dfd, "at.txt", &st, AT_SYMLINK_NOFOLLOW) == 0,
       "stat the leaf through the parent");
    ok(write_file_replace_at(dfd, "at.txt", "at\n", 3, st.st_mode, st.st_uid,
                             st.st_gid) == 0,
       "write_file_replace_at writes through the parent fd");
    char buf[16];
    ok(slurp(path, buf, sizeof buf) == 3 && strcmp(buf, "at\n") == 0,
       "the _at form wrote the expected bytes");
    close(dfd);
}

static void test_hash(void) {
    const char a[] = "alpha";
    const char b[] = "alphb";
    unsigned long long ha = meong_siphash24(a, 5);
    unsigned long long ha2 = meong_siphash24(a, 5);
    unsigned long long hb = meong_siphash24(b, 5);
    unsigned long long h5 = meong_siphash24(a, 4);
    ok(ha == ha2, "the same bytes hash the same within a process");
    ok(ha != hb, "a changed byte changes the hash");
    ok(ha != h5, "a changed length changes the hash");
    ok(ha != 0, "the hash is not a constant zero");
}

int main(void) {
    char dir[] = "/tmp/meong-write-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    test_replace(dir);
    test_at(dir);
    test_hash();

    /* The test leaves the tree for inspection only on failure. */
    if (!fails) {
        static const char *names[] = {
            "target.txt", "link.txt", "mode.txt", "at.txt", ".target.txt.mng",
        };
        char p[4096];
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            snprintf(p, sizeof p, "%s/%s", dir, names[i]);
            unlink(p);
        }
        rmdir(dir);
    }
    if (fails) {
        printf("%d FAILED (tree %s)\n", fails, dir);
        return 1;
    }
    printf("all write checks passed\n");
    return 0;
}
