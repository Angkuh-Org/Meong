/*
 * Edit publish-path fault injection through the real tool handler. The util
 * test covers the keyed writer directly; this one proves tool_run_edit leaves
 * the original byte-identical and no temp when openat/write/fchmod/fsync/
 * close/renameat fails after the human approved the card.
 */
#include "tools/tools.h"

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
static int fail_renameat;
static int temp_fd = -1;
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
    int fd = __real_openat(dfd, path, flags, mode);
    if (fd >= 0 && strstr(path, ".mng.") != NULL) {
        temp_fd = fd;
    }
    return fd;
}

int __wrap_close(int fd) {
    if (fail_close && fd == temp_fd) {
        fail_close = 0;
        errno = fail_errno;
        return -1;
    }
    return __real_close(fd);
}

ssize_t __wrap_write(int fd, const void *buf, size_t n) {
    if (fail_write && fd == temp_fd) {
        fail_write = 0;
        errno = fail_errno;
        return -1;
    }
    return __real_write(fd, buf, n);
}

int __wrap_fsync(int fd) {
    if (fail_fsync && fd == temp_fd) {
        fail_fsync = 0;
        errno = fail_errno;
        return -1;
    }
    return __real_fsync(fd);
}

int __wrap_fchmod(int fd, mode_t mode) {
    if (fail_fchmod && fd == temp_fd) {
        fail_fchmod = 0;
        errno = fail_errno;
        return -1;
    }
    return __real_fchmod(fd, mode);
}

int __wrap_unlinkat(int dfd, const char *path, int flags) {
    return __real_unlinkat(dfd, path, flags);
}

int __wrap_renameat(int olddfd, const char *oldp, int newdfd,
                    const char *newp) {
    if (fail_renameat) {
        fail_renameat = 0;
        errno = fail_errno;
        return -1;
    }
    return __real_renameat(olddfd, oldp, newdfd, newp);
}

static void reset(void) {
    fail_openat = 0;
    fail_write = 0;
    fail_fsync = 0;
    fail_close = 0;
    fail_fchmod = 0;
    fail_renameat = 0;
    temp_fd = -1;
    fail_errno = EIO;
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

static enum PermDecision auth_allow(void *ud, PermReq *req) {
    (void)ud;
    (void)req;
    return PERM_ALLOW;
}

static int run_edit(const char *cwd, Buf *out, ToolAsk *ask, char *err,
                    size_t cap) {
    static const char args[] =
        "{\"path\":\"f.txt\",\"old_string\":\"old\",\"new_string\":\"new\"}";
    buf_init(out);
    perm_ask_clear(ask);
    if (err && cap) {
        err[0] = '\0';
    }
    return tool_execute("edit", args, sizeof args - 1, cwd, NULL, auth_allow,
                        ask, out, err, cap);
}

static void run_case(const char *dir, int *flag, const char *label) {
    reset();
    char path[4096];
    snprintf(path, sizeof path, "%s/f.txt", dir);
    put(path, "old\n", 4, 0644);
    *flag = 1;
    Buf out;
    ToolAsk ask;
    char err[512];
    int rc = run_edit(dir, &out, &ask, err, sizeof err);
    char what[128];
    snprintf(what, sizeof what, "edit %s maps to TOOL_ERR", label);
    ok(rc == TOOL_ERR, what);
    char buf[16];
    ok(slurp(path, buf, sizeof buf) == 4 && strcmp(buf, "old\n") == 0,
       "the failed edit leaves the original bytes");
    ok(has_temp(dir) == 0, "the failed edit leaves no temp");
    buf_free(&out);
}

int main(void) {
    char root[] = "/tmp/meong-edit-fault-XXXXXX";
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    run_case(root, &fail_openat, "openat");
    run_case(root, &fail_write, "write");
    run_case(root, &fail_fchmod, "fchmod");
    run_case(root, &fail_fsync, "fsync");
    run_case(root, &fail_close, "close");
    run_case(root, &fail_renameat, "renameat");

    char path[4096];
    snprintf(path, sizeof path, "%s/f.txt", root);
    unlink(path);
    rmdir(root);
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("all edit fault checks passed\n");
    return 0;
}
