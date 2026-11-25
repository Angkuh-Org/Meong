/*
 * Syscall-order oracle: the atomic writer must fsync the temp before close,
 * close before rename, and fsync the directory after the rename. The fault
 * tests prove a failed syscall leaves no target; this proves the successful
 * order is the one the durability claim depends on.
 */
#include "util/util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
static int temp_fd = -1;
static char rename_old[64];
static char rename_new[64];

enum event {
    EV_OPEN,
    EV_FCHMOD,
    EV_WRITE,
    EV_FSYNC_TEMP,
    EV_FSYNC_DIR,
    EV_CLOSE,
    EV_RENAME,
    EV_MAX
};

static int events[32];
static int nevents;

static void ok(int cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        fails++;
    }
}

static void record(int ev) {
    if (nevents < (int)(sizeof events / sizeof events[0])) {
        events[nevents++] = ev;
    }
}

static int index_of(int ev) {
    for (int i = 0; i < nevents; i++) {
        if (events[i] == ev) {
            return i;
        }
    }
    return -1;
}

int __real_openat(int dfd, const char *path, int flags, ...);
int __real_close(int fd);
ssize_t __real_write(int fd, const void *buf, size_t n);
int __real_fsync(int fd);
int __real_fchmod(int fd, mode_t mode);
int __real_renameat(int olddfd, const char *oldp, int newdfd,
                    const char *newp);
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
    int fd = __real_openat(dfd, path, flags, mode);
    if (fd >= 0 && strstr(path, ".mng.") != NULL) {
        temp_fd = fd;
        record(EV_OPEN);
    }
    return fd;
}

int __wrap_fchmod(int fd, mode_t mode) {
    if (fd == temp_fd) {
        record(EV_FCHMOD);
    }
    return __real_fchmod(fd, mode);
}

ssize_t __wrap_write(int fd, const void *buf, size_t n) {
    if (fd == temp_fd) {
        record(EV_WRITE);
    }
    return __real_write(fd, buf, n);
}

int __wrap_fsync(int fd) {
    record(fd == temp_fd ? EV_FSYNC_TEMP : EV_FSYNC_DIR);
    return __real_fsync(fd);
}

int __wrap_close(int fd) {
    if (fd == temp_fd) {
        record(EV_CLOSE);
        temp_fd = -1;
    }
    return __real_close(fd);
}

int __wrap_renameat(int olddfd, const char *oldp, int newdfd,
                    const char *newp) {
    snprintf(rename_old, sizeof rename_old, "%s", oldp);
    snprintf(rename_new, sizeof rename_new, "%s", newp);
    record(EV_RENAME);
    return __real_renameat(olddfd, oldp, newdfd, newp);
}

int __wrap_meong_renameat2_noreplace(int dfd, const char *oldn,
                                     const char *newn) {
    snprintf(rename_old, sizeof rename_old, "%s", oldn);
    snprintf(rename_new, sizeof rename_new, "%s", newn);
    record(EV_RENAME);
    return __real_meong_renameat2_noreplace(dfd, oldn, newn);
}

static void reset(void) {
    temp_fd = -1;
    rename_old[0] = '\0';
    rename_new[0] = '\0';
    nevents = 0;
}

static void check_order(const char *label, int has_fchmod) {
    int o = index_of(EV_OPEN);
    int c = index_of(EV_FCHMOD);
    int w = index_of(EV_WRITE);
    int ft = index_of(EV_FSYNC_TEMP);
    int cl = index_of(EV_CLOSE);
    int r = index_of(EV_RENAME);
    int fd = index_of(EV_FSYNC_DIR);
    char what[128];
    snprintf(what, sizeof what, "%s: temp fsync precedes close", label);
    ok(ft >= 0 && cl >= 0 && ft < cl, what);
    snprintf(what, sizeof what, "%s: close precedes rename", label);
    ok(cl >= 0 && r >= 0 && cl < r, what);
    snprintf(what, sizeof what, "%s: rename precedes dir fsync", label);
    ok(r >= 0 && fd >= 0 && r < fd, what);
    snprintf(what, sizeof what, "%s: write precedes temp fsync", label);
    ok(w >= 0 && ft >= 0 && w < ft, what);
    snprintf(what, sizeof what, "%s: open precedes write", label);
    ok(o >= 0 && w >= 0 && o < w, what);
    if (has_fchmod) {
        snprintf(what, sizeof what, "%s: fchmod precedes write", label);
        ok(c >= 0 && w >= 0 && c < w, what);
    }
    snprintf(what, sizeof what, "%s: rename source is the keyed temp", label);
    ok(strncmp(rename_old, ".mng.", 5) == 0, what);
    snprintf(what, sizeof what, "%s: rename target is the base name", label);
    ok(strcmp(rename_new, "f.txt") == 0, what);
}

int main(void) {
    char root[] = "/tmp/meong-order-XXXXXX";
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/f.txt", root);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || write(fd, "old\n", 4) != 4) {
        perror("fixture");
        return 2;
    }
    close(fd);
    chmod(path, 0644);

    int dfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ok(dfd >= 0, "the parent dir opens");
    reset();
    ok(write_file_replace_at_keyed(dfd, "f.txt", "new\n", 4, 0644, geteuid(),
                                   getegid()) == 0,
       "keyed replace succeeds");
    check_order("replace", 1);
    close(dfd);

    reset();
    dfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ok(write_file_create_at(dfd, "g.txt", "new\n", 4, 0644) == 0,
       "create succeeds");
    int r = index_of(EV_RENAME);
    int fd2 = index_of(EV_FSYNC_DIR);
    int cl2 = index_of(EV_CLOSE);
    int ft2 = index_of(EV_FSYNC_TEMP);
    ok(ft2 >= 0 && cl2 >= 0 && ft2 < cl2, "create: temp fsync precedes close");
    ok(cl2 >= 0 && r >= 0 && cl2 < r, "create: close precedes publish");
    ok(r >= 0 && fd2 >= 0 && r < fd2, "create: publish precedes dir fsync");
    ok(strncmp(rename_old, ".mng.", 5) == 0,
       "create: publish source is the keyed temp");
    ok(strcmp(rename_new, "g.txt") == 0, "create: publish target is the base");
    close(dfd);

    unlink(path);
    snprintf(path, sizeof path, "%s/g.txt", root);
    unlink(path);
    rmdir(root);
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("all order checks passed\n");
    return 0;
}
