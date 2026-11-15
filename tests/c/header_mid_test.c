/*
 * Locks the header chip clamp: a model id longer than the header budget must
 * become a UTF-8-safe prefix plus an ellipsis inside cap, and the return value
 * must be display columns, not bytes. The old bug cut by bytes and let the
 * chip write its trailing space past the buffer.
 */
#include "ui/ui.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "view.c"

static int fails;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

static int cols_of(const char *s) {
    size_t n = strlen(s);
    size_t i = 0;
    int cols = 0;
    while (i < n) {
        wchar_t wc;
        int k = utf8_next(s + i, n - i, &wc);
        if (k <= 0) {
            return -1;
        }
        int w = wcwidth(wc);
        if (w < 0) {
            w = 1;
        }
        cols += w;
        i += (size_t)k;
    }
    return cols;
}

/* The ellipsis must be the last thing in buf; hand back the prefix length. */
static int tail_is(const char *buf, const char *ell, size_t *prefix) {
    size_t n = strlen(buf);
    size_t eb = strlen(ell);
    if (n < eb || memcmp(buf + n - eb, ell, eb) != 0) {
        return 0;
    }
    *prefix = n - eb;
    return 1;
}

/* cap is 256; anything at or past it means the clamp wrote out of bounds. */
static int sentinel_intact(const char *buf) {
    for (size_t i = 256; i < 260; i++) {
        if ((unsigned char)buf[i] != 0xA5) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    (void)setlocale(LC_CTYPE, "C.UTF-8");
    char buf[260];
    char want[260];
    char id[401];

    /* Fits: report 0 and leave the buffer alone. */
    memset(buf, 0xA5, sizeof buf);
    memset(want, 0xA5, sizeof want);
    check(header_mid(buf, 256, "mock/split", 30, 1) == 0,
          "fit returned a cut width");
    check(memcmp(buf, want, sizeof buf) == 0, "fit touched the buffer");

    /* Exact fit: strlen + 2 is still whole (n <= midmax - 2). */
    memset(buf, 0xA5, sizeof buf);
    memset(want, 0xA5, sizeof want);
    check(header_mid(buf, 256, "mock/split", 12, 1) == 0,
          "exact-fit id was cut");
    check(memcmp(buf, want, sizeof buf) == 0, "exact fit touched the buffer");

    memset(id, 'a', 400);
    id[400] = '\0';

    /* The old bug: 400 bytes into a 256-byte buffer with a 300 budget.
     * 252 prefix bytes + 3-byte ellipsis + NUL, display width 253. */
    memset(buf, 0xA5, sizeof buf);
    int ret = header_mid(buf, 256, id, 300, 1);
    check(ret == 253, "unicode cut width");
    check(strlen(buf) == 255, "unicode cut length");
    check(memcmp(buf + 252, "…", 3) == 0, "unicode ellipsis is not at 252");
    check(sentinel_intact(buf), "unicode cut wrote past cap-1");

    /* ASCII ellipsis: one byte buys one more column. */
    memset(buf, 0xA5, sizeof buf);
    ret = header_mid(buf, 256, id, 300, 0);
    check(ret == 255, "ascii cut width");
    check(strlen(buf) == 255, "ascii cut length");
    check(buf[254] == '~', "ascii ellipsis is not at 254");
    check(sentinel_intact(buf), "ascii cut wrote past cap-1");

    /* UTF-8 boundary: 251 'a', then a 2-byte codepoint, then more. A byte
     * cut would leave half of the codepoint in the prefix. */
    memset(id, 'a', 251);
    memcpy(id + 251, "\xC3\xA9", 2); /* é */
    memset(id + 253, 'b', 100);
    id[353] = '\0';
    memset(buf, 0xA5, sizeof buf);
    ret = header_mid(buf, 256, id, 300, 1);
    check(strlen(buf) <= 255, "utf8 cut overflows cap");
    size_t prefix = 0;
    check(tail_is(buf, "…", &prefix), "utf8 cut lost the ellipsis");
    check(prefix == 251, "utf8 cut did not stop before the 2-byte codepoint");
    char saved = buf[prefix];
    buf[prefix] = '\0';
    int cols = cols_of(buf);
    buf[prefix] = saved;
    check(cols == 251, "utf8 prefix is not 251 columns");
    check(ret == cols + 1, "utf8 cut returned bytes, not columns");
    check(ret != (int)strlen(buf), "utf8 cut width equals the byte count");

    /* Wide codepoints hit the byte cap on a codepoint boundary. 84 x 3-byte
     * wide chars fill 252 bytes; their display width is locale dependent,
     * so compare against wcwidth instead of a hardcoded 168. */
    for (int i = 0; i < 100; i++) {
        memcpy(id + i * 3, "界", 3);
    }
    id[300] = '\0';
    memset(buf, 0xA5, sizeof buf);
    ret = header_mid(buf, 256, id, 300, 1);
    check(strlen(buf) == 255, "cjk cut length");
    check(tail_is(buf, "…", &prefix), "cjk cut lost the ellipsis");
    check(prefix == 252, "cjk cut split a codepoint");
    saved = buf[prefix];
    buf[prefix] = '\0';
    cols = cols_of(buf);
    buf[prefix] = saved;
    check(cols > 0, "cjk prefix columns");
    check(ret == cols + 1, "cjk cut returned bytes, not columns");
    check(ret != (int)strlen(buf), "cjk cut width equals the byte count");

    printf(fails ? "FAILED: %d header check(s)\n" : "HEADER CHECKS OK\n",
           fails);
    return fails != 0;
}
