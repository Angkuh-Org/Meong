/*
 * Fault injection for the two atomic writers. The create path must never
 * replace, must fall back to linkat only on ENOSYS/EINVAL/EOPNOTSUPP, and must
 * leave no temp on every ordinary failure. The overwrite path must never use
 * NOREPLACE and must leave the original byte-identical.
 */
#include "util/util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
static int fail_openat;
static int fail_write;
static int fail_fsync;
static int fail_close;
static int fail_fchmod;
static int fail_unlinkat;
static int fail_renameat;
static int noreplace_errno;
static int fail_linkat_errno;
static int saw_noreplace;
static int saw_linkat;
static int saw_rename;
static int fail_errno = EIO;

static void ok(int cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        fails++;
    }
}

int __real_openat(int dfd, const char *path, int flags, ...);
int __real_close(int fd);
ssize_t __real_write(int fd, const void *buf, size_t n);
int __real_fsync(int fd);
int __real_fchmod(int fd, mode_t mode);
int __real_unlinkat(int dfd, const char *path, int flags);
int __real_renameat(int olddfd, const char *oldp, int newdfd,
                    const char *newp);
int __real_linkat(int olddfd, const char *oldp, int newdfd,
                  const char *newp, int flags);
int __real_meong_renameat2_noreplace(int dfd, const char *oldn,
                                     const char *newn);

int __wrap_openat(int dfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (fail_openat && strstr(path, ".mng.") != NULL) {
        fail_openat = 0;
        errno = fail_errno;
        return -1;
    }
    return __real_openat(dfd, path, flags, mode);
}

int __wrap_close(int fd) {
    if (fail_close) {
        fail_close = 0;
        errno = fail_errno;
        return -1;
    }
    return __real_close(fd);
}

ssize_t __wrap_write(int fd, const void *buf, size_t n) {
    if (fail_write) {
        fail_write = 0;
        errno = fail_errno;
        return -1;
    }
    return __real_write(fd, buf, n);
}

int __wrap_fsync(int fd) {
    if (fail_fsync) {
        fail_fsync = 0;
        errno = fail_errno;
        return -1;
    }
    return __real_fsync(fd);
}

int __wrap_fchmod(int fd, mode_t mode) {
    if (fail_fchmod) {
        fail_fchmod = 0;
        errno = fail_errno;
        return -1;
    }
    return __real_fchmod(fd, mode);
}

int __wrap_unlinkat(int dfd, const char *path, int flags) {
    if (fail_unlinkat) {
        fail_unlinkat = 0;
        errno = fail_errno;
        return -1;
    }
    return __real_unlinkat(dfd, path, flags);
}

int __wrap_renameat(int olddfd, const char *oldp, int newdfd,
                    const char *newp) {
    if (fail_renameat) {
        fail_renameat = 0;
        errno = fail_errno;
        return -1;
    }
    saw_rename = 1;
    return __real_renameat(olddfd, oldp, newdfd, newp);
}

int __wrap_linkat(int olddfd, const char *oldp, int newdfd,
                  const char *newp, int flags) {
    if (fail_linkat_errno) {
        int e = fail_linkat_errno;
        fail_linkat_errno = 0;
        errno = e;
        return -1;
    }
    saw_linkat = 1;
    return __real_linkat(olddfd, oldp, newdfd, newp, flags);
}

int __wrap_meong_renameat2_noreplace(int dfd, const char *oldn,
                                     const char *newn) {
    saw_noreplace = 1;
    if (noreplace_errno) {
        int e = noreplace_errno;
        noreplace_errno = 0;
        errno = e;
        return -1;
    }
    return __real_meong_renameat2_noreplace(dfd, oldn, newn);
}

static void reset(void) {
    fail_openat = 0;
    fail_write = 0;
    fail_fsync = 0;
    fail_close = 0;
    fail_fchmod = 0;
    fail_unlinkat = 0;
    fail_renameat = 0;
    noreplace_errno = 0;
    fail_linkat_errno = 0;
    saw_noreplace = 0;
    saw_linkat = 0;
    saw_rename = 0;
    fail_errno = EIO;
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

static void put(const char *path, const char *text, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        perror(path);
        exit(2);
    }
    if (write(fd, text, strlen(text)) != (ssize_t)strlen(text)) {
        perror(path);
        exit(2);
    }
    close(fd);
    chmod(path, mode);
}

static void test_create_ok(const char *dir) {
    reset();
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ok(write_file_create_at(dfd, "ok.txt", "hello\n", 6, 0644) == 0,
       "create publishes");
    char buf[32];
    char path[4096];
    snprintf(path, sizeof path, "%s/ok.txt", dir);
    ok(slurp(path, buf, sizeof buf) == 6 && strcmp(buf, "hello\n") == 0,
       "create writes the exact bytes");
    ok(has_temp(dir) == 0, "create leaves no temp");
    close(dfd);
}

static void test_create_refuses_existing(const char *dir) {
    reset();
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char path[4096];
    snprintf(path, sizeof path, "%s/exist.txt", dir);
    put(path, "old\n", 0644);
    errno = 0;
    int r = write_file_create_at(dfd, "exist.txt", "new\n", 4, 0644);
    ok(r == -1 && errno == EEXIST, "create refuses an existing target");
    char buf[16];
    ok(slurp(path, buf, sizeof buf) == 4 && strcmp(buf, "old\n") == 0,
       "the refused target is unchanged");
    ok(has_temp(dir) == 0, "the refusal leaves no temp");
    close(dfd);
}

static void test_create_fallback(const char *dir, int e, const char *label) {
    reset();
    noreplace_errno = e;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char name[64];
    snprintf(name, sizeof name, "fb-%d.txt", e);
    int r = write_file_create_at(dfd, name, "x\n", 2, 0644);
    char what[128];
    snprintf(what, sizeof what, "create falls back to linkat on %s", label);
    ok(r == 0 && saw_noreplace && saw_linkat, what);
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    char buf[8];
    ok(slurp(path, buf, sizeof buf) == 2 && strcmp(buf, "x\n") == 0,
       "the fallback target is exact");
    struct stat st;
    ok(stat(path, &st) == 0 && st.st_nlink == 1,
       "the fallback leaves one link");
    ok(has_temp(dir) == 0, "the fallback removes the temp");
    close(dfd);
}

static void test_create_eperm(const char *dir) {
    reset();
    noreplace_errno = EPERM;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    errno = 0;
    int r = write_file_create_at(dfd, "eperm.txt", "x\n", 2, 0644);
    ok(r == -1 && errno == EPERM && saw_noreplace && !saw_linkat,
       "EPERM is not retried through linkat");
    char path[4096];
    snprintf(path, sizeof path, "%s/eperm.txt", dir);
    struct stat st;
    ok(stat(path, &st) != 0, "the EPERM target is absent");
    ok(has_temp(dir) == 0, "the EPERM failure leaves no temp");
    close(dfd);
}

static void test_create_fault(const char *dir, int *flag, const char *name,
                              const char *label) {
    reset();
    *flag = 1;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    errno = 0;
    int r = write_file_create_at(dfd, name, "x\n", 2, 0644);
    char what[128];
    snprintf(what, sizeof what, "create %s maps to an error", label);
    ok(r == -1 && errno == fail_errno, what);
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    struct stat st;
    ok(stat(path, &st) != 0, "the failed create leaves no target");
    ok(has_temp(dir) == 0, "the failed create leaves no temp");
    close(dfd);
}

static void test_replace_ok(const char *dir) {
    reset();
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char path[4096];
    snprintf(path, sizeof path, "%s/replace.txt", dir);
    put(path, "old\n", 0640);
    ok(write_file_replace_at_keyed(dfd, "replace.txt", "new\n", 4, 0640,
                                   geteuid(), getegid()) == 0,
       "keyed replace publishes");
    char buf[16];
    ok(slurp(path, buf, sizeof buf) == 4 && strcmp(buf, "new\n") == 0,
       "keyed replace writes the exact bytes");
    struct stat st;
    ok(stat(path, &st) == 0 && (st.st_mode & 07777) == 0640,
       "keyed replace preserves the mode");
    ok(saw_rename && !saw_noreplace, "replace uses renameat, not NOREPLACE");
    ok(has_temp(dir) == 0, "replace leaves no temp");
    close(dfd);
}

static void test_replace_fault(const char *dir, int *flag, const char *label) {
    reset();
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char path[4096];
    snprintf(path, sizeof path, "%s/replace-%s.txt", dir, label);
    put(path, "old\n", 0644);
    *flag = 1;
    errno = 0;
    int r = write_file_replace_at_keyed(dfd, strrchr(path, '/') + 1, "new\n",
                                        4, 0644, geteuid(), getegid());
    char what[128];
    snprintf(what, sizeof what, "replace %s returns an error", label);
    ok(r == -1 && errno == fail_errno, what);
    char buf[16];
    ok(slurp(path, buf, sizeof buf) == 4 && strcmp(buf, "old\n") == 0,
       "the failed replace keeps the original bytes");
    ok(has_temp(dir) == 0, "the failed replace leaves no temp");
    close(dfd);
}

static void test_replace_no_takeover(const char *dir) {
    reset();
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char user[4096];
    char target[4096];
    snprintf(user, sizeof user, "%s/.foo.mng", dir);
    snprintf(target, sizeof target, "%s/foo", dir);
    put(user, "keep\n", 0644);
    put(target, "old\n", 0644);
    ok(write_file_replace_at_keyed(dfd, "foo", "new\n", 4, 0644, geteuid(),
                                   getegid()) == 0,
       "keyed replace runs beside a user .foo.mng");
    char buf[16];
    ok(slurp(user, buf, sizeof buf) == 5 && strcmp(buf, "keep\n") == 0,
       "a user .foo.mng survives a keyed replace");
    close(dfd);
}

int main(void) {
    char root[] = "/tmp/meong-fault-XXXXXX";
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    umask(022);
    test_create_ok(root);
    test_create_refuses_existing(root);
    test_create_fallback(root, ENOSYS, "ENOSYS");
    test_create_fallback(root, EINVAL, "EINVAL");
    test_create_fallback(root, EOPNOTSUPP, "EOPNOTSUPP");
    test_create_eperm(root);
    test_create_fault(root, &fail_openat, "open.txt", "openat");
    test_create_fault(root, &fail_write, "write.txt", "write");
    test_create_fault(root, &fail_fsync, "fsync.txt", "fsync");
    test_create_fault(root, &fail_close, "close.txt", "close");
    test_create_fault(root, &fail_fchmod, "chmod.txt", "fchmod");
    test_replace_ok(root);
    test_replace_fault(root, &fail_write, "write");
    test_replace_fault(root, &fail_fsync, "fsync");
    test_replace_fault(root, &fail_close, "close");
    test_replace_fault(root, &fail_renameat, "renameat");
    test_replace_no_takeover(root);
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd) != 0) {
        return 2;
    }
    if (fails) {
        fprintf(stderr, "%d fault test(s) failed\n", fails);
        return 1;
    }
    printf("write fault: all checks passed\n");
    return 0;
}
