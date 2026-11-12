#ifndef MEONG_UI_H
#define MEONG_UI_H

#include <curses.h>
#include <stddef.h>
#include <wchar.h>

#include "app/app.h"

enum {
    P_USER = 1,
    P_AI,
    P_DIM,
    P_ACCENT,
    P_ERROR,
    P_CODE,
    P_FENCE,
    P_NOTE,
    P_BORDER,
    P_OK,
    P_WAIT,
    P_HEADER,
    P_BRAND,
    P_SEL,
    P_BASE,
    P_PANEL,
    P_PANEL_DIM,
    P_PANEL_ACCENT,
    P_PANEL_BORDER,
    P_SHADOW,
    P_HOK,
    P_HWAIT,
    P_CHIP_CC,
    P_CHIP_OC,
    P_CHIP_KILO,
    P_CHIP_BOB,
    P_CHIP_MISTRAL,
    P_CHIP_OTHER,
};

static inline attr_t pair_attr(const App *a, int pair) {
    return a->color ? COLOR_PAIR(pair) : 0;
}

static inline const char *sym_user(const App *a) {
    return a->unicode ? "◑" : ">";
}

static inline const char *sym_ai(const App *a) {
    return a->unicode ? "✦" : "+";
}

static inline const char *sym_think(const App *a) {
    return a->unicode ? "○" : "o";
}

static inline const char *sym_status(const App *a) {
    return a->unicode ? "●" : "*";
}

static inline const char *sym_err(const App *a) {
    return a->unicode ? "!" : "!";
}

static inline const char *sym_gutter(const App *a) {
    return a->unicode ? "▎" : "|";
}

static inline const char *sym_reason_gutter(const App *a) {
    return a->unicode ? "┆" : ":";
}

static inline const char *sym_cursor(const App *a) {
    return a->unicode ? "▌" : "#";
}

static inline const char *sym_dot(const App *a) {
    return a->unicode ? "·" : "-";
}

static inline const char *sym_arrow_down(const App *a) {
    return a->unicode ? "↓" : "v";
}

static inline const char *sym_arrow_up(const App *a) {
    return a->unicode ? "↑" : "^";
}

int ui_init(App *a);
void ui_draw(App *a);

void backend_label(const char *model, char *out, size_t cap);
int chip_pair(const char *label);
void fmt_elapsed(double sec, char *out, size_t cap);
int chat_height(const App *a, int rows);

void draw_scrollbar(App *a, int y0, int h, int rows, int cols);
void draw_splash(App *a, int y0, int h, int cols);
void draw_chat(App *a, int rows, int cols);
int draw_slice(int y, int x, const char *s, int off, int len, int maxx,
               int pair, int attr, int *endx);

void picker_open(App *a);
void picker_close(App *a);
void picker_filter_backspace(App *a);
void picker_filter_clear(App *a);
void picker_filter_append(App *a, char c);
void picker_draw(App *a, int rows, int cols);
void help_draw(App *a, int rows, int cols);

void history_free(App *a);
void history_add(App *a, const wchar_t *s, size_t n);
void history_move(App *a, int dir);
void input_insert(App *a, wchar_t wc);
void input_backspace(App *a);
void input_del(App *a);
void input_kill_word(App *a);
void input_reset(App *a);
void chat_scroll(App *a, int lines);
void scroll_bottom(App *a);

#endif
