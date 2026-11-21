/*
 * The preview scanner is a security-adjacent display filter: it decides what
 * the permission card shows and how many controls it reports. The ASCII fast
 * path in preview_control_count() must not change a single byte or count, so
 * this file compares it with a scalar copy of the original logic over ASCII,
 * ESC, UTF-8, bidi, invalid bytes and small output caps.
 */
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void ok(int cond, const char *what) {
    if (!cond) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

static int ref_is_drop(wchar_t wc) {
    return wc < 32 || wc == 0x7f || (wc >= 0x80 && wc <= 0x9f) ||
           (wc >= 0x202a && wc <= 0x202e) ||
           (wc >= 0x2066 && wc <= 0x2069) || wc == 0x200e ||
           wc == 0x200f || wc == 0x061c || (wc >= 0x200b && wc <= 0x200d) ||
           wc == 0xfeff;
}

static size_t ref_ellipsis(char *out, size_t o, size_t cap) {
    if (o + 4 <= cap) {
        memcpy(out + o, "...", 3);
        return o + 3;
    }
    return o;
}

static size_t ref_sanitize(const char *s, size_t n, char *out, size_t cap,
                           unsigned flags, size_t *dropped) {
    size_t o = 0;
    size_t i = 0;
    size_t dropn = 0;
    if (!out || cap == 0) {
        return 0;
    }
    while (i < n && s[i] != '\n' && s[i] != '\r') {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            dropn++;
            i++;
            if (i < n && s[i] == '[') {
                i++;
                while (i < n && !((unsigned char)s[i] >= 0x40 &&
                                  (unsigned char)s[i] <= 0x7e)) {
                    i++;
                }
                if (i < n) {
                    i++;
                }
            } else if (i < n && s[i] == ']') {
                i++;
                while (i < n) {
                    if (s[i] == 0x07) {
                        i++;
                        break;
                    }
                    if (s[i] == 0x1b && i + 1 < n && s[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
            } else if (i < n) {
                i++;
            }
            continue;
        }
        wchar_t wc = 0;
        int k = utf8_next(s + i, n - i, &wc);
        if (k <= 0) {
            k = 1;
            wc = 0xFFFD;
        }
        if (wc == '\t' && (flags & PV_TABS)) {
            if (o + 5 > cap) {
                o = ref_ellipsis(out, o, cap);
                break;
            }
            memcpy(out + o, "    ", 4);
            o += 4;
            i += (size_t)k;
            continue;
        }
        if (ref_is_drop(wc)) {
            dropn++;
            if (flags & PV_REPLACE) {
                if (o + 2 > cap) {
                    o = ref_ellipsis(out, o, cap);
                    break;
                }
                out[o++] = '?';
            }
            i += (size_t)k;
            continue;
        }
        if (o + (size_t)k + 1 > cap) {
            o = ref_ellipsis(out, o, cap);
            break;
        }
        if (k == 1 && wc == 0xFFFD && c >= 0x80) {
            out[o++] = '?';
        } else {
            memcpy(out + o, s + i, (size_t)k);
            o += (size_t)k;
        }
        i += (size_t)k;
    }
    out[o] = '\0';
    if (dropped) {
        *dropped += dropn;
    }
    return o;
}

static size_t ref_control_count(const char *s, size_t n) {
    size_t count = 0;
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            count++;
            i++;
            if (i < n && s[i] == '[') {
                i++;
                while (i < n && !((unsigned char)s[i] >= 0x40 &&
                                  (unsigned char)s[i] <= 0x7e)) {
                    i++;
                }
                if (i < n) {
                    i++;
                }
            } else if (i < n && s[i] == ']') {
                i++;
                while (i < n) {
                    if (s[i] == 0x07) {
                        i++;
                        break;
                    }
                    if (s[i] == 0x1b && i + 1 < n && s[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
            } else if (i < n) {
                i++;
            }
            continue;
        }
        wchar_t wc = 0;
        int k = utf8_next(s + i, n - i, &wc);
        if (k <= 0) {
            count++;
            i++;
            continue;
        }
        if (wc == '\n' || wc == '\r' || wc == '\t') {
            i += (size_t)k;
            continue;
        }
        if (ref_is_drop(wc)) {
            count++;
        }
        i += (size_t)k;
    }
    return count;
}

static void pair(const char *label, const char *s, size_t n, size_t cap,
                 unsigned flags) {
    char a[512];
    char b[512];
    size_t da = 0;
    size_t db = 0;
    size_t ra = preview_sanitize_line(s, n, a, cap, flags, &da);
    size_t rb = ref_sanitize(s, n, b, cap, flags, &db);
    if (ra != rb || da != db || memcmp(a, b, ra + 1) != 0) {
        printf("FAIL sanitize %s (cap %zu flags %u): got %zu/%zu '%s' "
               "want %zu/%zu '%s'\n",
               label, cap, flags, ra, da, a, rb, db, b);
        fails++;
    }
    size_t ca = preview_control_count(s, n);
    size_t cb = ref_control_count(s, n);
    if (ca != cb) {
        printf("FAIL count %s: got %zu want %zu\n", label, ca, cb);
        fails++;
    }
}

static void corpus(const char *label, const char *s) {
    size_t n = strlen(s);
    pair(label, s, n, 512, PV_TABS | PV_REPLACE | PV_ELLIPSIS);
    pair(label, s, n, 8, PV_TABS | PV_REPLACE | PV_ELLIPSIS);
    pair(label, s, n, 4, PV_REPLACE | PV_ELLIPSIS);
    pair(label, s, n, 1, PV_ELLIPSIS);
}

int main(void) {
    corpus("empty", "");
    corpus("printable", "hello world 0123456789 ~");
    corpus("c0", "\001\002\037x");
    corpus("del", "a\177b");
    corpus("whitespace", "a\tb\nc\rd");
    corpus("esc-csi", "a\033[31mred\033[0mz");
    corpus("esc-osc-bel", "a\033]0;title\007z");
    corpus("esc-osc-st", "a\033]0;title\033\\z");
    corpus("esc-bare", "a\033z");
    corpus("utf8-valid", "caf\303\251");
    corpus("utf8-invalid", "a\377b");
    corpus("utf8-cont", "\200\201");
    corpus("bidi", "a\342\200\256b\342\200\256c");
    corpus("zero-width", "a\342\200\213b\342\200\215c");
    corpus("bom", "\357\273\277x");
    corpus("mixed", "a\t\033[1m\342\200\256\377\177z");

    char out[8];
    size_t dropped = 0;
    ok(preview_sanitize_line("x", 1, NULL, 8, 0, NULL) == 0,
       "null output returns 0");
    ok(preview_sanitize_line("x", 1, out, 0, 0, NULL) == 0,
       "zero cap returns 0");
    ok(preview_sanitize_line("a\tb", 3, out, sizeof out, PV_TABS, &dropped) ==
           6 &&
           dropped == 0 && strcmp(out, "a    b") == 0,
       "tab expands to four spaces");
    ok(preview_sanitize_line("a\tb", 3, out, sizeof out, 0, &dropped) == 2 &&
           dropped == 1 && strcmp(out, "ab") == 0,
       "tab without PV_TABS is dropped");
    ok(preview_control_count("\n\r\t", 3) == 0,
       "line endings and tab are not controls");
    const char esc[] = "\001\033[31m\177";
    ok(preview_control_count(esc, sizeof esc - 1) == 3,
       "C0 and DEL count, ESC sequence counts once");

    size_t n = 24u << 10;
    char *big = malloc(n);
    if (!big) {
        return 2;
    }
    for (size_t i = 0; i < n; i++) {
        big[i] = (i % 97 == 96) ? '\n' : 'x';
    }
    ok(preview_control_count(big, n) == 0,
       "24 KiB ASCII has no controls");
    big[100] = 0x1b;
    big[101] = '[';
    big[102] = '3';
    big[103] = '1';
    big[104] = 'm';
    ok(preview_control_count(big, n) == 1,
       "one ESC in 24 KiB counts once");
    free(big);

    if (fails) {
        fprintf(stderr, "%d preview test(s) failed\n", fails);
        return 1;
    }
    printf("preview: all checks passed\n");
    return 0;
}
