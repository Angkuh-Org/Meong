#include "ui/ui.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "chat/chat.h"

static int err_char_width(wchar_t wc) {
    if (wc == '\t') {
        return 4;
    }
    int w = wcwidth(wc);
    return w < 0 ? 1 : w;
}

/*
 * Draw or count wrapped rows, one row per loop; `draw` 0 stops before the
 * terminal writes so the caller can size the panel first.
 */
static int err_wrap(int y, int ymax, int x, int width, const char *s,
                    int draw) {
    if (width <= 0) {
        return y;
    }
    while (s && *s && y < ymax) {
        /* TODO(perf): strlen(s) rescans the remaining text every row, O(n^2)
         * on newline-heavy errors. Hoisting n was measured at +30 insn and
         * +136 B text; keep this until the ncurses-bound panel is hot. */
        size_t n = strlen(s);
        size_t i = 0;
        size_t row_end = n;
        size_t last_space = 0;
        int have_space = 0;
        int col = 0;
        int saw = 0;
        int newline = 0;
        while (i < n) {
            wchar_t wc;
            int k = utf8_next(s + i, n - i, &wc);
            if (k <= 0) {
                k = 1;
                wc = (wchar_t)(unsigned char)s[i];
            }
            if (wc == '\n') {
                row_end = i;
                newline = 1;
                break;
            }
            int cw = err_char_width(wc);
            if (saw && col + cw > width) {
                row_end = have_space ? last_space : i;
                break;
            }
            if (wc == ' ') {
                last_space = i;
                have_space = 1;
            }
            col += cw;
            i += (size_t)k;
            saw = 1;
        }
        if (row_end == 0 && !newline) {
            /* A glyph wider than the panel still has to advance. */
            wchar_t wc;
            int k = utf8_next(s, n, &wc);
            row_end = k > 0 ? (size_t)k : 1;
        }
        if (draw && row_end > 0) {
            mvaddnstr(y, x, s, (int)row_end);
        }
        s += row_end + (size_t)newline;
        while (*s == ' ') {
            s++;
        }
        y++;
    }
    return y;
}

static void error_activate(App *a) {
    int action = a->error_action;
    a->error_open = 0;
    a->dirty = 1;
    if (action == ERR_RETRY_MODELS) {
        refresh_models(a);
    } else {
        retry_last(a);
    }
}

void error_open(App *a, const char *text, int action) {
    const char *src = text && *text ? text : "request failed";
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < sizeof a->error_text; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\t') {
            c = ' ';
        } else if ((c < 32 && c != '\n') || c == 127) {
            continue;
        }
        a->error_text[o++] = (char)c;
    }
    a->error_text[o] = '\0';
    if (o == 0) {
        snprintf(a->error_text, sizeof a->error_text, "request failed");
    }
    a->error_open = 1;
    a->error_sel = 0;
    a->error_action = action;
    a->last_failed = 1;
    a->picker_open = 0;
    a->help_open = 0;
    a->pet_open = 0;
    a->model_open = 0;
    a->dirty = 1;
}

void error_close(App *a) {
    if (!a->error_open) {
        return;
    }
    a->error_open = 0;
    a->dirty = 1;
}

void error_key(App *a, wint_t wch, int special) {
    if (!a->error_open) {
        return;
    }
    if (special) {
        if (wch == KEY_UP || wch == KEY_LEFT) {
            a->error_sel = 0;
        } else if (wch == KEY_DOWN || wch == KEY_RIGHT) {
            a->error_sel = 1;
        } else if (wch == KEY_ENTER) {
            if (a->error_sel == 1) {
                error_close(a);
            } else {
                error_activate(a);
            }
            return;
        }
        a->dirty = 1;
        return;
    }
    switch (wch) {
    case 27:
    case 3:
        error_close(a);
        break;
    case 12:
        clearok(stdscr, TRUE);
        a->dirty = 1;
        break;
    case '\n':
    case '\r':
    case ' ':
        if (a->error_sel == 1) {
            error_close(a);
        } else {
            error_activate(a);
        }
        break;
    case '\t':
        a->error_sel = !a->error_sel;
        a->dirty = 1;
        break;
    case 'r':
    case 'R':
        a->error_sel = 0;
        error_activate(a);
        break;
    case 'c':
    case 'C':
        error_close(a);
        break;
    default:
        break;
    }
}

int error_click(App *a, int y, int x) {
    if (!a->error_open) {
        return 0;
    }
    if (a->error_y0 >= 0 && y >= a->error_y0 && y <= a->error_y1 &&
        x >= a->error_x0 && x <= a->error_x1) {
        if (y == a->error_retry_y) {
            a->error_sel = 0;
            error_activate(a);
        } else if (y == a->error_close_y) {
            a->error_sel = 1;
            error_close(a);
        } else {
            a->dirty = 1;
        }
    }
    return 1;
}

static void error_button(App *a, int y, int px, int w, const char *label,
                         int sel) {
    if (sel) {
        attr_t sa = a->color ? pair_attr(a, P_SEL) : (attr_t)A_REVERSE;
        wattrset(stdscr, sa);
        mvhline(y, px + 1, ' ', w - 2);
        wattrset(stdscr, sa | A_BOLD);
        mvaddstr(y, px + 2, a->unicode ? "› " : "> ");
        mvaddstr(y, px + 4, label);
    } else {
        wattrset(stdscr, pair_attr(a, P_PANEL));
        mvhline(y, px + 1, ' ', w - 2);
        wattrset(stdscr, pair_attr(a, P_PANEL_ACCENT) | A_BOLD);
        mvaddstr(y, px + 2, "  ");
        mvaddstr(y, px + 4, label);
    }
}

void error_draw(App *a, int rows, int cols) {
    if (!a->error_open) {
        return;
    }
    int w = cols - 4;
    if (w > 64) {
        w = 64;
    }
    if (w < 30) {
        return;
    }
    int width = w - 6;
    int yy = err_wrap(0, INT_MAX, 0, width, a->error_text, 0);
    if (yy < 1) {
        yy = 1;
    }
    int h = yy + 6;
    if (h > rows - 2) {
        h = rows - 2;
    }
    if (h < 7) {
        return;
    }
    int px = (cols - w) / 2;
    int py = (rows - h) / 2;
    if (py + h - 1 >= rows - 4) {
        py = rows - 4 - h;
        if (py < 1) {
            py = 1;
        }
    }
    a->error_y0 = py;
    a->error_y1 = py + h - 1;
    a->error_x0 = px;
    a->error_x1 = px + w - 1;
    panel_frame(a, px, py, w, h, " request failed ", 1);

    int y = py + 2;
    wattrset(stdscr, pair_attr(a, P_PANEL_ACCENT) | A_BOLD);
    mvaddstr(y, px + 2, sym_err(a));
    err_wrap(y, py + h - 4, px + 4, width, a->error_text, 1);

    int by = py + h - 3;
    a->error_retry_y = by;
    a->error_close_y = by + 1;
    error_button(a, by, px, w, "retry", a->error_sel == 0);
    error_button(a, by + 1, px, w, "close", a->error_sel == 1);
    a->cursor_on = 0;
}
