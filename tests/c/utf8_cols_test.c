/*
 * Locks the header width primitive: sizes are display columns, not bytes,
 * and a tail cut never splits a UTF-8 codepoint.
 */
#include "util/util.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int fails;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

static int tail_width(const char *s, int cols, size_t *off) {
    *off = utf8_tail(s, cols);
    return utf8_cols(s + *off);
}

int main(void) {
    (void)setlocale(LC_CTYPE, "C.UTF-8");

    check(utf8_cols("") == 0, "empty width");
    check(utf8_cols("abc") == 3, "ascii width");
    check(utf8_cols("…") == 1, "ellipsis is one column");
    check(utf8_cols("● streaming") == 11, "status is eleven columns");
    check(utf8_cols("\xFF") == 1, "invalid byte is one column");
    check(utf8_cols("a\xFF" "b") == 3,
          "invalid byte stays inside the string");

    int w = wcwidth(L'界');
    if (w < 0) {
        w = 1;
    }
    check(utf8_cols("界") == w, "wide codepoint follows wcwidth");

    size_t off;
    check(tail_width("a…b", 2, &off) == 2 && off == 1,
          "tail keeps two columns");
    check(tail_width("a…b", 1, &off) == 1 && off == 4,
          "tail drops the ellipsis");
    check(utf8_tail("a…b", 3) == 0, "whole string fits");
    check(utf8_tail("a…b", 0) == strlen("a…b"), "zero columns is empty");

    check(tail_width("xéy", 2, &off) == 2 && off == 1,
          "two-byte tail boundary");
    check(tail_width("xéy", 1, &off) == 1 && off == 3, "two-byte tail drops");

    off = utf8_tail("界界", w);
    check(off == 3, "wide tail cut on a codepoint boundary");
    check(utf8_cols("界界" + off) == w, "wide tail width");

    printf(fails ? "FAILED: %d utf8 column check(s)\n" : "UTF8 COLS OK\n",
           fails);
    return fails != 0;
}
