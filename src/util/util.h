#ifndef MEONG_UTIL_H
#define MEONG_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <wchar.h>

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_clear(Buf *b);
void buf_reserve(Buf *b, size_t extra);
void buf_add(Buf *b, const void *data, size_t len);
void buf_adds(Buf *b, const char *s);
void buf_addc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_json_str(Buf *b, const char *s, size_t len);
char *buf_take(Buf *b);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

double now_sec(void);
int utf8_next(const char *s, size_t n, wchar_t *out);
int utf8_encode(wchar_t wc, char out[4]);
int utf8_cols(const char *s);
size_t utf8_tail(const char *s, int cols);
char *wcs_to_utf8(const wchar_t *s, size_t n);
int str_contains_ci(const char *hay, const char *needle);
int str_ieq(const char *a, const char *b);
char *trim_ws(char *s);
void fmt_count(long long v, char *out, size_t cap);

/* Preview-only sanitizer shared by edit and write. */
enum {
    PV_TABS = 1u << 0,     /* render a tab as four spaces */
    PV_REPLACE = 1u << 1,  /* replace a dropped control with '?' */
    PV_ELLIPSIS = 1u << 2, /* append ... when the cap is hit */
};
size_t preview_sanitize_line(const char *s, size_t n, char *out, size_t cap,
                             unsigned flags, size_t *dropped);
size_t preview_control_count(const char *s, size_t n);

char *read_file(const char *path, size_t *len_out);
int write_file_atomic(const char *path, const char *data, size_t len);

/*
 * Replace an existing file with `data` atomically: same-directory hidden temp,
 * O_EXCL|O_NOFOLLOW, chown-then-chmod, write, fsync, renameat, best-effort
 * directory fsync. Refuses a non-regular leaf, a hardlink and a foreign owner.
 * No directory creation. The `_at` form takes the already-walked parent fd.
 */
int write_file_replace(const char *path, const char *data, size_t len);
int write_file_replace_at(int dfd, const char *base, const char *data,
                          size_t len, mode_t mode, uid_t uid, gid_t gid);

/*
 * Create BASE in an already-walked parent. Never replaces: publication is
 * renameat2(RENAME_NOREPLACE), with a linkat fallback only when the filesystem
 * or kernel does not support the flag. No directory creation, no final
 * symlink follow, no O_CREAT at the final name. MODE is the final mode.
 */
int write_file_create_at(int dfd, const char *base, const char *data,
                         size_t len, mode_t mode);

/*
 * Replace an existing file with a keyed, non-embedding temp name. edit and
 * write use this variant; write_file_replace_at remains the legacy path for
 * write_file_replace()/tests.
 */
int write_file_replace_at_keyed(int dfd, const char *base, const char *data,
                                size_t len, mode_t mode, uid_t uid, gid_t gid);

/* Non-static so the fault-injection tests can --wrap it. */
int meong_renameat2_noreplace(int dfd, const char *oldn, const char *newn);

/* Keyed SipHash-2-4 over the whole buffer; used for approval identity. */
unsigned long long meong_siphash24(const void *data, size_t len);

int mkdir_p(const char *path);

#endif
