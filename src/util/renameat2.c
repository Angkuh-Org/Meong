/*
 * Raw renameat2 wrapper. Kept in its own translation unit so the fault tests
 * can --wrap it; a call inside util.c would be resolved within that object and
 * never reach the linker's wrap.
 */
#include "util/util.h"

#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif

int meong_renameat2_noreplace(int dfd, const char *oldn, const char *newn) {
#if defined(SYS_renameat2)
    return (int)syscall(SYS_renameat2, dfd, oldn, dfd, newn,
                        RENAME_NOREPLACE);
#else
    (void)dfd;
    (void)oldn;
    (void)newn;
    errno = ENOSYS;
    return -1;
#endif
}
