#ifndef MEONG_UTIL_H
#define MEONG_UTIL_H

#include <stddef.h>
#include <stdint.h>
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
char *wcs_to_utf8(const wchar_t *s, size_t n);
int str_contains_ci(const char *hay, const char *needle);
int str_ieq(const char *a, const char *b);
char *trim_ws(char *s);
void fmt_count(long long v, char *out, size_t cap);

char *read_file(const char *path, size_t *len_out);
int write_file_atomic(const char *path, const char *data, size_t len);
int mkdir_p(const char *path);

#endif
