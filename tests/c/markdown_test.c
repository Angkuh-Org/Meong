/*
 * Streaming markdown oracle.  Every case feeds the same bytes in chunks;
 * visible text must lose the syntax, and the display metadata must describe
 * the block structure the layout consumes.
 */
#include "app/app.h"
#include "ui/md.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static int checks;

static void check(int cond, const char *what) {
    checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

static char *visible(const Msg *m) {
    Buf b;
    buf_init(&b);
    size_t i = 0;
    while (i < m->dlen) {
        unsigned char c = (unsigned char)m->disp[i];
        if (c == MD_CMD && i + 1 < m->dlen) {
            unsigned char cmd = (unsigned char)m->disp[i + 1];
            i += 2;
            if ((cmd & MD_META) && i < m->dlen) {
                i++;
            }
            continue;
        }
        buf_addc(&b, (char)c);
        i++;
    }
    return buf_take(&b);
}

static int has_style(const Msg *m, unsigned mask) {
    for (size_t i = 0; i + 1 < m->dlen; i++) {
        if ((unsigned char)m->disp[i] == MD_CMD &&
            (unsigned char)m->disp[i + 1] == mask) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    int kind;
    int hang;
} Meta;

static int metas(const Msg *m, Meta *out, int max) {
    int n = 0;
    size_t pos = 0;
    while (pos < m->dlen && n < max) {
        size_t nl = pos;
        while (nl < m->dlen && m->disp[nl] != '\n') {
            nl++;
        }
        int hang = 0;
        size_t skip = 0;
        int kind = md_line_meta(m->disp + pos, nl - pos, &hang, &skip);
        out[n].kind = kind;
        out[n].hang = hang;
        n++;
        if (nl >= m->dlen) {
            break;
        }
        pos = nl + 1;
    }
    return n;
}

typedef struct {
    App app;
    Msg *m;
} Fixture;

static void fixture_init(Fixture *f) {
    memset(f, 0, sizeof *f);
    int idx = msg_add(&f->app, ROLE_ASSISTANT, "");
    f->m = &f->app.msgs[idx];
    f->m->streaming = 1;
}

static void feed(Fixture *f, const char *s) {
    size_t old = f->m->dlen;
    char *prev = NULL;
    if (old > 0) {
        prev = xmalloc(old);
        memcpy(prev, f->m->disp, old);
    }
    msg_append(f->m, s, strlen(s));
    md_sync(f->m, 1);
    if (old > 0) {
        check(f->m->dlen >= old && memcmp(f->m->disp, prev, old) == 0,
              "display prefix stays append-only");
    }
    free(prev);
}

static void finish(Fixture *f) {
    size_t old = f->m->dlen;
    char *prev = NULL;
    if (old > 0) {
        prev = xmalloc(old);
        memcpy(prev, f->m->disp, old);
    }
    f->m->streaming = 0;
    md_sync(f->m, 1);
    if (old > 0) {
        check(f->m->dlen >= old && memcmp(f->m->disp, prev, old) == 0,
              "finished display only appends");
    }
    free(prev);
}

static void fixture_free(Fixture *f) {
    msgs_clear(&f->app);
    free(f->app.msgs);
}

static void case_inline_split(void) {
    Fixture f;
    fixture_init(&f);
    feed(&f, "This is **bo");
    feed(&f, "ld**, *it");
    feed(&f, "alic* and `co");
    feed(&f, "de`.");
    finish(&f);
    char *v = visible(f.m);
    check(strcmp(v, "This is bold, italic and code.\n") == 0,
          "inline markers are removed");
    check(has_style(f.m, MD_BOLD), "bold style is emitted");
    check(has_style(f.m, MD_ITALIC), "italic style is emitted");
    check(has_style(f.m, MD_CODE), "inline code style is emitted");
    check(strstr(v, "**") == NULL && strstr(v, "`") == NULL,
          "inline syntax never reaches the screen");
    free(v);
    fixture_free(&f);
}

static void case_blocks(void) {
    Fixture f;
    fixture_init(&f);
    feed(&f, "# Title\n\n- one\n");
    feed(&f, "  - two\n\n> quoted\n");
    feed(&f, "---\n");
    finish(&f);
    char *v = visible(f.m);
    check(strstr(v, "Title") != NULL, "heading text survives");
    check(strstr(v, "#") == NULL, "heading hash is hidden");
    check(strstr(v, "\xE2\x80\xA2 one") != NULL, "bullet glyph is drawn");
    check(strstr(v, "  \xE2\x80\xA2 two") != NULL,
          "nested bullet keeps its indent");
    check(strstr(v, "\xE2\x94\x82 quoted") != NULL, "quote rail is drawn");
    Meta md[8];
    int n = metas(f.m, md, 8);
    int head = 0;
    int list = 0;
    int quote = 0;
    int rule = 0;
    int list_hang = 0;
    for (int i = 0; i < n; i++) {
        if (md[i].kind == MDL_HEAD) {
            head++;
        } else if (md[i].kind == MDL_LIST) {
            list++;
            if (md[i].hang == 2) {
                list_hang = 1;
            }
        } else if (md[i].kind == MDL_QUOTE) {
            quote++;
        } else if (md[i].kind == MDL_RULE) {
            rule++;
        }
    }
    check(head == 1, "one heading line");
    check(list == 2, "two list lines");
    check(list_hang, "list hanging indent reaches the layout");
    check(quote == 1, "one quote line");
    check(rule == 1, "one horizontal rule");
    free(v);
    fixture_free(&f);
}

static void case_fence_split(void) {
    Fixture f;
    fixture_init(&f);
    feed(&f, "``");
    feed(&f, "`c\nint x = ");
    feed(&f, "1;\n``");
    feed(&f, "`\n");
    finish(&f);
    char *v = visible(f.m);
    check(strstr(v, "c\nint x = 1;\n") != NULL, "fence keeps code and language");
    check(strstr(v, "```") == NULL, "fence markers are hidden");
    Meta md[8];
    int n = metas(f.m, md, 8);
    check(n == 3, "fence open, code, close");
    check(md[0].kind == MDL_FENCE_OPEN, "opening fence metadata");
    check(md[1].kind == MDL_CODE, "code line metadata");
    check(md[2].kind == MDL_FENCE_CLOSE, "closing fence metadata");
    free(v);
    fixture_free(&f);
}

static void case_truncated_fence(void) {
    Fixture f;
    fixture_init(&f);
    feed(&f, "```c\nint x = 1;");
    finish(&f);
    char *v = visible(f.m);
    check(strstr(v, "c\nint x = 1;") != NULL,
          "a truncated fence keeps its code");
    Meta md[8];
    int n = metas(f.m, md, 8);
    check(n == 3, "truncated fence still gets a bottom row");
    check(md[0].kind == MDL_FENCE_OPEN, "truncated fence opens");
    check(md[1].kind == MDL_CODE, "truncated fence keeps code metadata");
    check(md[2].kind == MDL_FENCE_CLOSE, "truncated fence is closed at finish");
    free(v);
    fixture_free(&f);
}

static void case_lists_and_strike(void) {
    Fixture f;
    fixture_init(&f);
    feed(&f, "1. first\n2. second\n- [ ] todo\n");
    feed(&f, "- [x] done\n~~gone~~ text\n");
    finish(&f);
    char *v = visible(f.m);
    check(strstr(v, "1. first\n2. second\n") != NULL,
          "ordered list markers survive");
    check(strstr(v, "\xE2\x98\x90 todo") != NULL, "empty task box is drawn");
    check(strstr(v, "\xE2\x98\x91 done") != NULL, "checked task box is drawn");
    check(strstr(v, "gone text") != NULL, "strike text keeps its content");
    check(strstr(v, "~~") == NULL, "strike markers are hidden");
    check(has_style(f.m, MD_STRIKE), "strike style is emitted");
    Meta md[8];
    int n = metas(f.m, md, 8);
    int list = 0;
    for (int i = 0; i < n; i++) {
        if (md[i].kind == MDL_LIST) {
            list++;
        }
    }
    check(list == 4, "ordered and task rows all carry list metadata");
    free(v);
    fixture_free(&f);
}

static void case_pending_cap(void) {
    Fixture f;
    fixture_init(&f);
    size_t n = 9000;
    char *big = xmalloc(n + 1);
    memset(big, ' ', n);
    big[0] = '-';
    big[1] = '-';
    big[2] = '-';
    big[n] = '\0';
    big[4500] = '\0';
    feed(&f, big);
    big[4500] = ' ';
    feed(&f, big + 4500);
    finish(&f);
    char *v = visible(f.m);
    check(strlen(v) > 0, "a huge ambiguous line still produces visible text");
    free(v);
    free(big);
    fixture_free(&f);
}

static void case_table(void) {
    Fixture f;
    fixture_init(&f);
    feed(&f, "| a | b |\n|---");
    feed(&f, "|---|\n| 1 | 2 |\n");
    finish(&f);
    char *v = visible(f.m);
    check(strstr(v, "\xE2\x94\x82 a \xE2\x94\x82 b \xE2\x94\x82") != NULL,
          "table cells use a rail");
    check(strstr(v, "---") == NULL, "table separator is not visible");
    Meta md[8];
    int n = metas(f.m, md, 8);
    int table = 0;
    int rule = 0;
    for (int i = 0; i < n; i++) {
        if (md[i].kind == MDL_TABLE) {
            table++;
        } else if (md[i].kind == MDL_RULE) {
            rule++;
        }
    }
    check(table == 2, "two visible table rows");
    check(rule == 1, "table separator becomes a rule");
    free(v);
    fixture_free(&f);
}

static void case_link(void) {
    Fixture f;
    fixture_init(&f);
    feed(&f, "[Do");
    feed(&f, "cs](http");
    feed(&f, "s://example.com) and [x]\n");
    finish(&f);
    char *v = visible(f.m);
    check(strcmp(v, "Docs and [x]\n") == 0, "link label replaces its syntax");
    check(strstr(v, "example.com") == NULL, "link URL is hidden");
    check(has_style(f.m, MD_LINK), "link style is emitted");
    free(v);
    fixture_free(&f);
}

static void case_escape_and_code(void) {
    Fixture f;
    fixture_init(&f);
    feed(&f, "\\*not italic\\* and `a**b`\n");
    finish(&f);
    char *v = visible(f.m);
    check(strcmp(v, "*not italic* and a**b\n") == 0,
          "escapes and inline-code bodies stay literal");
    free(v);
    fixture_free(&f);
}

static void case_unclosed(void) {
    Fixture f;
    fixture_init(&f);
    feed(&f, "a *");
    finish(&f);
    char *v = visible(f.m);
    check(strcmp(v, "a *\n") == 0, "a trailing star is flushed literally");
    free(v);
    fixture_free(&f);

    Fixture g;
    fixture_init(&g);
    feed(&g, "**open");
    finish(&g);
    v = visible(g.m);
    check(strcmp(v, "open\n") == 0, "an unclosed strong span keeps its text");
    check(has_style(g.m, MD_BOLD), "an unclosed strong span keeps its style");
    free(v);
    fixture_free(&g);
}

int main(void) {
    case_inline_split();
    case_blocks();
    case_fence_split();
    case_truncated_fence();
    case_lists_and_strike();
    case_pending_cap();
    case_table();
    case_link();
    case_escape_and_code();
    case_unclosed();
    printf(fails ? "FAILED: %d/%d markdown check(s)\n"
                 : "MARKDOWN CHECKS OK (%d)\n",
           fails ? fails : checks, checks);
    return fails != 0;
}
