#include "ui/ui.h"

#include <stdlib.h>
#include <string.h>

static void panel_frame(App *a, int px, int py, int w, int h,
                        const char *title) {
    if (a->color && COLORS >= 256) {
        wattrset(stdscr, pair_attr(a, P_SHADOW));
        for (int y = py + 1; y <= py + h; y++) {
            for (int x = px + 1; x <= px + w; x++) {
                if (y >= 0 && y < LINES && x >= 0 && x < COLS) {
                    mvaddch(y, x, ' ');
                }
            }
        }
    }
    if (a->color) {
        wattrset(stdscr, pair_attr(a, P_PANEL));
    } else {
        wattrset(stdscr, 0);
    }
    for (int y = py; y < py + h; y++) {
        mvhline(y, px, ' ', w);
    }
    wattrset(stdscr, pair_attr(a, P_PANEL_BORDER) | A_BOLD);
    if (a->unicode) {
        mvaddstr(py, px, "┌");
        for (int i = 1; i < w - 1; i++) {
            mvaddstr(py, px + i, "─");
        }
        mvaddstr(py, px + w - 1, "┐");
        for (int y = py + 1; y < py + h - 1; y++) {
            mvaddstr(y, px, "│");
            mvaddstr(y, px + w - 1, "│");
        }
        mvaddstr(py + h - 1, px, "└");
        for (int i = 1; i < w - 1; i++) {
            mvaddstr(py + h - 1, px + i, "─");
        }
        mvaddstr(py + h - 1, px + w - 1, "┘");
    } else {
        mvaddstr(py, px, "+");
        for (int i = 1; i < w - 1; i++) {
            mvaddch(py, px + i, '-');
        }
        mvaddstr(py, px + w - 1, "+");
        for (int y = py + 1; y < py + h - 1; y++) {
            mvaddstr(y, px, "|");
            mvaddstr(y, px + w - 1, "|");
        }
        mvaddstr(py + h - 1, px, "+");
        for (int i = 1; i < w - 1; i++) {
            mvaddch(py + h - 1, px + i, '-');
        }
        mvaddstr(py + h - 1, px + w - 1, "+");
    }
    if (title && *title) {
        wattrset(stdscr, pair_attr(a, P_PANEL_ACCENT) | A_BOLD);
        mvaddnstr(py, px + 3, title, w - 6);
    }
}

static void picker_recompute(App *a) {
    a->pick_n = 0;
    for (int i = 0; i < a->nmodels; i++) {
        if (a->picker_filter_len == 0 ||
            str_contains_ci(a->models[i], a->picker_filter)) {
            a->pick_matches[a->pick_n++] = i;
        }
    }
    if (a->pick_n == 0) {
        a->picker_sel = 0;
    } else if (a->picker_sel >= a->pick_n) {
        a->picker_sel = a->pick_n - 1;
    } else if (a->picker_sel < 0) {
        a->picker_sel = 0;
    }
}

void picker_filter_backspace(App *a) {
    if (a->picker_filter_len <= 0) {
        return;
    }
    a->picker_filter_len--;
    while (a->picker_filter_len > 0 &&
           (a->picker_filter[a->picker_filter_len] & 0xC0) == 0x80) {
        a->picker_filter_len--;
    }
    a->picker_filter[a->picker_filter_len] = '\0';
    picker_recompute(a);
}

void picker_filter_clear(App *a) {
    a->picker_filter_len = 0;
    a->picker_filter[0] = '\0';
    picker_recompute(a);
}

void picker_filter_append(App *a, char c) {
    if (a->picker_filter_len + 1 >= (int)sizeof a->picker_filter) {
        return;
    }
    a->picker_filter[a->picker_filter_len++] = c;
    a->picker_filter[a->picker_filter_len] = '\0';
    a->picker_sel = 0;
    a->picker_scroll = 0;
    picker_recompute(a);
}

void picker_open(App *a) {
    a->picker_open = 1;
    a->help_open = 0;
    a->picker_filter[0] = '\0';
    a->picker_filter_len = 0;
    a->picker_sel = 0;
    a->picker_scroll = 0;
    free(a->pick_matches);
    a->pick_matches = xcalloc((size_t)(a->nmodels > 0 ? a->nmodels : 1),
                              sizeof *a->pick_matches);
    picker_recompute(a);
    for (int i = 0; i < a->pick_n; i++) {
        if (strcmp(a->models[a->pick_matches[i]], a->model) == 0) {
            a->picker_sel = i;
            break;
        }
    }
    a->dirty = 1;
}

void picker_close(App *a) {
    a->picker_open = 0;
    a->dirty = 1;
}

void picker_draw(App *a, int rows, int cols) {
    int w = cols - 4;
    if (w > 78) {
        w = 78;
    }
    int h = rows - 4;
    if (h > 22) {
        h = 22;
    }
    if (w < 24 || h < 8) {
        return;
    }
    int px = (cols - w) / 2;
    int py = (rows - h) / 2;
    panel_frame(a, px, py, w, h, " choose a model ");
    int fy = py + 1;
    wattrset(stdscr, pair_attr(a, P_PANEL_DIM));
    mvaddstr(fy, px + 2, "filter");
    int fx = px + 9;
    int fmax = px + w - 2;
    if (a->picker_filter_len == 0) {
        wattrset(stdscr, pair_attr(a, P_PANEL_DIM));
        mvaddnstr(fy, fx, "type to filter", fmax - fx + 1);
        a->cur_y = fy;
        a->cur_x = fx;
    } else {
        int avail = fmax - fx + 1;
        int len = a->picker_filter_len;
        int start = 0;
        while (len - start > avail) {
            start++;
        }
        wattrset(stdscr, pair_attr(a, P_PANEL));
        mvaddnstr(fy, fx, a->picker_filter + start, avail);
        a->cur_y = fy;
        a->cur_x = fx + (len - start);
        if (a->cur_x > fmax) {
            a->cur_x = fmax;
        }
    }
    wattrset(stdscr, pair_attr(a, P_PANEL_DIM));
    if (a->unicode) {
        mvaddstr(py + 2, px, "├");
        for (int i = 1; i < w - 1; i++) {
            mvaddstr(py + 2, px + i, "─");
        }
        mvaddstr(py + 2, px + w - 1, "┤");
    } else {
        mvaddstr(py + 2, px, "+");
        for (int i = 1; i < w - 1; i++) {
            mvaddch(py + 2, px + i, '-');
        }
        mvaddstr(py + 2, px + w - 1, "+");
    }
    int list_y = py + 3;
    int list_h = h - 5;
    if (list_h < 0) {
        list_h = 0;
    }
    if (a->picker_sel < a->picker_scroll) {
        a->picker_scroll = a->picker_sel;
    }
    if (list_h > 0 && a->picker_sel >= a->picker_scroll + list_h) {
        a->picker_scroll = a->picker_sel - list_h + 1;
    }
    if (a->pick_n == 0) {
        wattrset(stdscr, pair_attr(a, P_PANEL_DIM));
        mvaddnstr(list_y, px + 3, "no matches", w - 6);
    }
    for (int row = 0; row < list_h; row++) {
        int idx = a->picker_scroll + row;
        int y = list_y + row;
        if (idx >= a->pick_n) {
            break;
        }
        int mi = a->pick_matches[idx];
        const char *id = a->models[mi];
        int sel = idx == a->picker_sel;
        if (sel) {
            attr_t selattr =
                a->color ? pair_attr(a, P_SEL) : (attr_t)A_REVERSE;
            wattrset(stdscr, selattr);
            mvhline(y, px + 1, ' ', w - 2);
            wattrset(stdscr, selattr | A_BOLD);
            mvaddstr(y, px + 2, a->unicode ? "› " : "> ");
            mvaddnstr(y, px + 4, id, w - 6);
        } else {
            wattrset(stdscr, pair_attr(a, P_PANEL));
            mvhline(y, px + 1, ' ', w - 2);
            mvaddstr(y, px + 2, "  ");
            char label[32];
            backend_label(id, label, sizeof label);
            const char *slash = strchr(id, '/');
            int plen = slash ? (int)(slash - id) + 1 : 0;
            wattrset(stdscr, pair_attr(a, chip_pair(label)) | A_BOLD);
            mvaddnstr(y, px + 4, id, plen);
            wattrset(stdscr, pair_attr(a, P_PANEL));
            int rest = w - 6 - plen;
            if (rest > 0) {
                mvaddnstr(y, px + 4 + plen, id + plen, rest);
            }
        }
    }
    wattrset(stdscr, pair_attr(a, P_PANEL_DIM));
    char info[96];
    int shown = a->pick_n > 0 ? a->picker_sel + 1 : 0;
    snprintf(info, sizeof info, "%d/%d  %s%s  enter  esc", shown, a->pick_n,
             sym_arrow_up(a), sym_arrow_down(a));
    mvaddnstr(py + h - 2, px + 3, info, w - 6);
    if (a->picker_scroll > 0) {
        int on_sel = a->picker_scroll == a->picker_sel;
        if (a->color) {
            wattrset(stdscr, pair_attr(a, on_sel ? P_SEL : P_PANEL_DIM));
        } else {
            wattrset(stdscr, on_sel ? A_REVERSE : A_DIM);
        }
        mvaddstr(list_y, px + w - 2, sym_arrow_up(a));
    }
    if (a->picker_scroll + list_h < a->pick_n) {
        int on_sel = a->picker_scroll + list_h - 1 == a->picker_sel;
        if (a->color) {
            wattrset(stdscr, pair_attr(a, on_sel ? P_SEL : P_PANEL_DIM));
        } else {
            wattrset(stdscr, on_sel ? A_REVERSE : A_DIM);
        }
        mvaddstr(list_y + list_h - 1, px + w - 2, sym_arrow_down(a));
    }
    a->cursor_on = 1;
}

void help_draw(App *a, int rows, int cols) {
    int w = cols - 6;
    if (w > 72) {
        w = 72;
    }
    int h = rows - 4;
    if (h > 24) {
        h = 24;
    }
    if (w < 30 || h < 10) {
        return;
    }
    int px = (cols - w) / 2;
    int py = (rows - h) / 2;
    panel_frame(a, px, py, w, h, " Meong · help ");
    const char *lines[] = {
        "commands",
        "  /model [id]    pick a model, or switch directly",
        "  /models        refresh the gateway model list",
        "  /system [p]    show, set, or clear ('off') the system prompt",
        "  /clear         forget the conversation",
        "  /retry         regenerate the last answer",
        "  /help          this panel",
        "  /quit          leave Meong",
        "",
        "keys",
        "  enter          send, or queue while replying",
        "  tab            model picker",
        "  pgup / pgdn    scroll the conversation",
        "  ctrl-g / ctrl-t jump to bottom / top",
        "  ctrl-c         cancel a reply, or quit when idle",
        "  ctrl-l         redraw",
        "",
        "press any key to close",
    };
    int n = (int)(sizeof lines / sizeof lines[0]);
    int y = py + 1;
    for (int i = 0; i < n && y < py + h - 1; i++, y++) {
        int header = (strcmp(lines[i], "commands") == 0 ||
                      strcmp(lines[i], "keys") == 0);
        if (header) {
            wattrset(stdscr, pair_attr(a, P_PANEL_ACCENT) | A_BOLD);
        } else {
            wattrset(stdscr, pair_attr(a, P_PANEL));
        }
        mvaddnstr(y, px + 3, lines[i], w - 6);
    }
    a->cursor_on = 0;
}

