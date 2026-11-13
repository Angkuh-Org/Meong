#include "ui/ui.h"

#include <langinfo.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "api/api.h"
#include "chat/chat.h"

/* acid/cyber industrial: xterm-256 indices nearest to
 * base 101110, surface 222522, text d9ded6, accent b6ff00, muted 747b72 */
#define C_BASE 233
#define C_SURFACE 235
#define C_TEXT 253
#define C_ACCENT 154
#define C_MUTED 243
#define C_SHADOW 234

/* one quadrant filled clockwise; wall-clock phase, and the modulo below is
 * the table length, so adding a frame cannot read past the array */
#define SPIN_FRAMES(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define SPIN_HZ 5 /* 5 frames/s: one clockwise turn per 0.8 s */
static const char *spin_utf8[] = {"▖", "▘", "▝", "▗"};
static const char *spin_ascii[] = {"|", "/", "-", "\\"};

static void term_signal(int sig) {
    endwin();
    _exit(128 + sig);
}

static int detect_utf8(void) {
    const char *cs = nl_langinfo(CODESET);
    return cs && (str_ieq(cs, "UTF-8") || str_ieq(cs, "UTF8"));
}

static void init_pairs(void) {
    if (COLORS >= 256) {
        init_pair(P_BASE, C_TEXT, C_BASE);
        init_pair(P_USER, C_ACCENT, C_BASE);
        init_pair(P_AI, C_TEXT, C_BASE);
        init_pair(P_DIM, C_MUTED, C_BASE);
        init_pair(P_ACCENT, C_ACCENT, C_BASE);
        init_pair(P_ERROR, C_BASE, C_ACCENT);
        init_pair(P_CODE, C_ACCENT, C_BASE);
        init_pair(P_FENCE, C_MUTED, C_BASE);
        init_pair(P_NOTE, C_MUTED, C_BASE);
        init_pair(P_BORDER, C_MUTED, C_BASE);
        init_pair(P_OK, C_ACCENT, C_BASE);
        init_pair(P_WAIT, C_ACCENT, C_BASE);
        init_pair(P_HEADER, C_TEXT, C_SURFACE);
        init_pair(P_BRAND, C_ACCENT, C_SURFACE);
        init_pair(P_SEL, C_BASE, C_ACCENT);
        init_pair(P_PANEL, C_TEXT, C_SURFACE);
        init_pair(P_PANEL_DIM, C_MUTED, C_SURFACE);
        init_pair(P_PANEL_ACCENT, C_ACCENT, C_SURFACE);
        init_pair(P_PANEL_BORDER, C_MUTED, C_SURFACE);
        init_pair(P_SHADOW, C_SHADOW, C_SHADOW);
        init_pair(P_HOK, C_ACCENT, C_SURFACE);
        init_pair(P_HWAIT, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_CC, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_OC, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_KILO, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_BOB, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_MISTRAL, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_OTHER, C_ACCENT, C_SURFACE);
    } else {
        init_pair(P_BASE, COLOR_WHITE, -1);
        init_pair(P_USER, COLOR_GREEN, -1);
        init_pair(P_AI, COLOR_WHITE, -1);
        init_pair(P_DIM, COLOR_WHITE, -1);
        init_pair(P_ACCENT, COLOR_GREEN, -1);
        init_pair(P_ERROR, COLOR_RED, -1);
        init_pair(P_CODE, COLOR_GREEN, -1);
        init_pair(P_FENCE, COLOR_WHITE, -1);
        init_pair(P_NOTE, COLOR_WHITE, -1);
        init_pair(P_BORDER, COLOR_WHITE, -1);
        init_pair(P_OK, COLOR_GREEN, -1);
        init_pair(P_WAIT, COLOR_GREEN, -1);
        init_pair(P_HEADER, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_BRAND, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_SEL, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_PANEL, COLOR_WHITE, -1);
        init_pair(P_PANEL_DIM, COLOR_WHITE, -1);
        init_pair(P_PANEL_ACCENT, COLOR_GREEN, -1);
        init_pair(P_PANEL_BORDER, COLOR_WHITE, -1);
        init_pair(P_SHADOW, COLOR_WHITE, -1);
        init_pair(P_HOK, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_HWAIT, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_CC, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_OC, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_KILO, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_BOB, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_MISTRAL, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_OTHER, COLOR_BLACK, COLOR_GREEN);
    }
}

int ui_init(App *a) {
    setlocale(LC_ALL, "");
    int env_utf8 = detect_utf8();
    if (!env_utf8) {
        if (!setlocale(LC_ALL, "C.UTF-8")) {
            setlocale(LC_ALL, "en_US.UTF-8");
        }
    }
    if (!initscr()) {
        fprintf(stderr, "Meong: cannot initialize the terminal\n");
        return -1;
    }
    a->unicode = !a->ascii && env_utf8;
    if (getenv("MEONG_ASCII")) {
        a->unicode = 0;
    }
    if (a->color) {
        if (start_color() != OK || !has_colors()) {
            a->color = 0;
        } else {
            use_default_colors();
            init_pairs();
        }
    }
    raw();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(1);
    if (a->mouse) {
        mousemask(ALL_MOUSE_EVENTS, NULL);
        /* 0 disables click resolution, so clicks arrive as press events */
        mouseinterval(0);
    }
    signal(SIGTERM, term_signal);
    signal(SIGHUP, term_signal);
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void backend_label(const char *model, char *out, size_t cap) {
    const char *slash = strchr(model, '/');
    size_t n = slash ? (size_t)(slash - model) : strlen(model);
    if (n == 0 || n >= cap) {
        snprintf(out, cap, "ai");
        return;
    }
    char tmp[64];
    if (n >= sizeof tmp) {
        n = sizeof tmp - 1;
    }
    memcpy(tmp, model, n);
    tmp[n] = '\0';
    if (str_ieq(tmp, "commandcode") || str_ieq(tmp, "cc")) {
        snprintf(out, cap, "cc");
    } else if (str_ieq(tmp, "opencode") || str_ieq(tmp, "oc")) {
        snprintf(out, cap, "oc");
    } else if (str_ieq(tmp, "opencode-go")) {
        snprintf(out, cap, "oc-go");
    } else if (str_ieq(tmp, "kilocode") || str_ieq(tmp, "kilo")) {
        snprintf(out, cap, "kilo");
    } else if (str_ieq(tmp, "mistral")) {
        snprintf(out, cap, "mistral");
    } else if (str_ieq(tmp, "bob")) {
        snprintf(out, cap, "bob");
    } else {
        size_t tl = strlen(tmp);
        if (tl >= cap) {
            tl = cap - 1;
        }
        memcpy(out, tmp, tl);
        out[tl] = '\0';
    }
}

int chip_pair(const char *label) {
    if (str_ieq(label, "cc")) {
        return P_CHIP_CC;
    }
    if (str_ieq(label, "oc") || str_ieq(label, "oc-go")) {
        return P_CHIP_OC;
    }
    if (str_ieq(label, "kilo")) {
        return P_CHIP_KILO;
    }
    if (str_ieq(label, "bob")) {
        return P_CHIP_BOB;
    }
    if (str_ieq(label, "mistral")) {
        return P_CHIP_MISTRAL;
    }
    return P_CHIP_OTHER;
}

void fmt_elapsed(double sec, char *out, size_t cap) {
    if (sec < 0) {
        sec = 0;
    }
    if (sec < 10) {
        snprintf(out, cap, "%.1fs", sec);
    } else if (sec < 60) {
        snprintf(out, cap, "%.0fs", sec);
    } else {
        int m = (int)(sec / 60);
        int s = (int)sec % 60;
        snprintf(out, cap, "%dm%02ds", m, s);
    }
}

static void draw_header(App *a, int rows, int cols) {
    (void)rows;
    if (a->color) {
        attrset(pair_attr(a, P_HEADER));
        mvhline(0, 0, ' ', cols);
    } else {
        attrset(A_REVERSE);
        mvhline(0, 0, ' ', cols);
    }
    attr_t base = a->color ? pair_attr(a, P_HEADER) : A_REVERSE;
    if (a->color) {
        wattrset(stdscr, pair_attr(a, P_BRAND) | A_BOLD);
    } else {
        wattrset(stdscr, base | A_BOLD);
    }
    mvaddstr(0, 1, "Meong");
    a->brand_y = 0;
    a->brand_x0 = 1;
    a->brand_x1 = 8; /* the word plus two cells of touch padding */
    wattrset(stdscr, base | A_DIM);
    mvaddstr(0, 7, " | ");

    char prov[32];
    backend_label(a->model, prov, sizeof prov);

    /* TODO(fix): getcwd() fails for paths longer than 511 bytes, and this
     * runs every idle frame even though src/ never chdir()s. Cache the cwd
     * in App at startup (or refresh if a future command can change it), and
     * fall back to a shorter suffix instead of dropping to "ready". */
    char right[512];
    /* TODO(mem): right/fit/midbuf/prov put ~1.7 KB peak on the draw stack.
     * In-place right truncation was measured: -512 B frame, +16 insn and
     * +64 B text, so it was rejected. Revisit only if draw-stack size
     * becomes a constraint. */
    char fit[512];
    const char *show = right;
    int right_dim = 0;
    if (a->streaming) {
        snprintf(right, sizeof right, "%s streaming", sym_status(a));
    } else if (getcwd(right, sizeof right)) {
        right_dim = 1;
    } else {
        snprintf(right, sizeof right, "%s ready", sym_status(a));
    }
    int right_w = (int)strlen(right);

    int x = 10;
    int midmax = cols - x - 2;
    if (right_w > 0) {
        int reserve = right_w + 1;
        if (reserve > cols / 2) {
            reserve = cols / 2;
        }
        midmax -= reserve;
    }
    /* TODO(dead): unreachable while ui_draw gates cols >= UI_MIN_COLS (36):
     * 36 - 12 - 36/2 = 6. Keep as a guard only if the header geometry
     * changes; otherwise delete. */
    if (midmax < 6) {
        midmax = 6;
        right_w = 0;
    }
    const char *mid = a->model ? a->model : "";
    int midn = (int)strlen(mid);
    char midbuf[256];
    /* TODO(fix): when keep exceeds sizeof midbuf - ellipsis bytes, snprintf
     * truncates but midn stays keep, so the closing cell, mend and model_x1
     * run past the drawn text. Clamp keep and derive midn from the ellipsis
     * width -- not strlen(), because the UTF-8 ellipsis is 3 bytes/1 column. */
    if (midn > midmax - 2) {
        int keep = midmax - 2;
        snprintf(midbuf, sizeof midbuf, "%.*s%s", keep - 1, mid,
                 a->unicode ? "…" : "~");
        mid = midbuf;
        midn = keep;
    }
    int mstart = x;
    if (a->color) {
        wattrset(stdscr, pair_attr(a, chip_pair(prov)) | A_BOLD);
        mvaddch(0, x, ' ');
        mvaddstr(0, x + 1, mid);
        mvaddch(0, x + 1 + midn, ' ');
    } else {
        wattrset(stdscr, base | A_BOLD | A_REVERSE);
        mvaddch(0, x, '[');
        mvaddstr(0, x + 1, mid);
        mvaddch(0, x + 1 + midn, ']');
    }
    int mend = x + midn + 2;
    a->model_y = 0;
    a->model_x0 = mstart;
    a->model_x1 = mend;

    if (right_w > 0) {
        int avail = cols - 2 - mend;
        if (right_w > avail) {
            int keep = avail;
            if (keep > 1) {
                snprintf(fit, sizeof fit, "%s%s", a->unicode ? "…" : "~",
                         right + right_w - (keep - 1));
                show = fit;
                right_w = keep;
            } else {
                right_w = 0;
            }
        }
        if (right_w > 0) {
            if (a->color) {
                wattrset(stdscr, right_dim
                                      ? (base | A_DIM)
                                      : (pair_attr(a, P_HWAIT) | A_BOLD));
            } else {
                wattrset(stdscr, base | (right_dim ? A_DIM : A_BOLD));
            }
            mvaddstr(0, cols - 1 - right_w, show);
        }
    }
}

void draw_scrollbar(App *a, int y0, int h, int rows, int cols) {
    if (a->nlines <= h || h <= 0) {
        return;
    }
    int x = cols - 1;
    wattrset(stdscr, pair_attr(a, P_DIM));
    for (int i = 0; i < h; i++) {
        mvaddstr(y0 + i, x, a->unicode ? "│" : "|");
    }
    int thumb_h = (int)((long long)h * h / a->nlines);
    if (thumb_h < 1) {
        thumb_h = 1;
    }
    int denom = a->nlines - h;
    int pos = 0;
    if (denom > 0) {
        pos = (int)((long long)(h - thumb_h) * a->top / denom);
    }
    wattrset(stdscr, pair_attr(a, P_ACCENT) | A_BOLD);
    for (int i = 0; i < thumb_h; i++) {
        if (y0 + pos + i < y0 + h) {
            mvaddstr(y0 + pos + i, x, a->unicode ? "█" : "#");
        }
    }
    (void)rows;
}

void draw_splash(App *a, int y0, int h, int cols) {
    if (h < 6) {
        return;
    }
    int yc = y0 + (h - 8) / 2;
    if (yc < y0) {
        yc = y0;
    }
    const char *cat[3] = {" /\\_/\\ ", "( o.o )", " > ^ < "};
    int cx = (cols - 7) / 2;
    if (cx < 1) {
        cx = 1;
    }
    wattrset(stdscr, pair_attr(a, P_ACCENT) | A_BOLD);
    for (int i = 0; i < 3; i++) {
        mvaddstr(yc + i, cx, cat[i]);
    }
    int namey = yc + 4;
    if (namey >= y0 + h) {
        return;
    }
    wattrset(stdscr, pair_attr(a, P_ACCENT) | A_BOLD);
    mvaddstr(namey, (cols - 5) / 2, "Meong");
    wattrset(stdscr, pair_attr(a, P_DIM));
    const char *tag = "talk to anything behind PAIG";
    if ((int)strlen(tag) < cols) {
        mvaddstr(namey + 1, (cols - (int)strlen(tag)) / 2, tag);
    }
    if (namey + 3 < y0 + h) {
        const char *hint = "type a message below  |  tab chooses a model";
        if ((int)strlen(hint) < cols) {
            mvaddstr(namey + 3, (cols - (int)strlen(hint)) / 2, hint);
        }
    }
    if (a->models_warn[0] && namey + 5 < y0 + h) {
        wattrset(stdscr, pair_attr(a, P_ERROR) | A_BOLD);
        int avail = cols - 4;
        if (avail > 0) {
            mvaddnstr(namey + 5, 2, a->models_warn, avail);
        }
    }
}

static int status_rows(const App *a) {
    int n = 0;
    if (a->streaming) {
        n++;
    }
    if (a->nqueued > 0) {
        n += 2;
    }
    return n;
}

int chat_height(const App *a, int rows) {
    int h = rows - 5 - status_rows(a);
    return h > 1 ? h : 1;
}

static void draw_status(App *a, int rows, int cols) {
    int n = status_rows(a);
    if (n == 0) {
        return;
    }
    int y = rows - 4 - n;
    if (a->streaming) {
        double now = now_sec();
        unsigned long long tick = (unsigned long long)(now * SPIN_HZ);
        const char *spin =
            a->unicode ? spin_utf8[tick % SPIN_FRAMES(spin_utf8)]
                       : spin_ascii[tick % SPIN_FRAMES(spin_ascii)];
        char elapsed[32];
        char count[32];
        fmt_elapsed(now - a->gen_start, elapsed, sizeof elapsed);
        Msg *m = (a->gen_idx >= 0 && a->gen_idx < a->nmsg)
                     ? &a->msgs[a->gen_idx]
                     : NULL;
        fmt_count(m ? (long long)m->chars : 0, count, sizeof count);
        wattrset(stdscr, pair_attr(a, P_WAIT) | A_BOLD);
        mvaddstr(y, 2, spin);
        char info[80];
        snprintf(info, sizeof info, "%s  %s ch", elapsed, count);
        wattrset(stdscr, pair_attr(a, P_DIM));
        if (cols > 10) {
            mvaddnstr(y, 4, info, cols - 5);
        }
        y++;
    }
    if (a->nqueued > 0) {
        wattrset(stdscr, pair_attr(a, P_DIM));
        mvaddstr(y, 4, a->unicode ? "queue ↓" : "queue v");
        y++;
        int x = 4;
        int maxx = cols - 3;
        char more[32];
        more[0] = '\0';
        if (a->nqueued > 1) {
            snprintf(more, sizeof more, "+%d", a->nqueued - 1);
            maxx = cols - 4 - (int)strlen(more);
        }
        int endx = x;
        if (maxx >= x) {
            draw_slice(y, x, a->queued[0], 0, (int)strlen(a->queued[0]),
                       maxx, a->color ? pair_attr(a, P_DIM) : 0, 0, &endx);
        }
        if (more[0]) {
            int mx = cols - 2 - (int)strlen(more);
            wattrset(stdscr, pair_attr(a, P_DIM));
            if (mx > x) {
                mvaddstr(y, mx, more);
            }
        }
    }
}

static void draw_input(App *a, int rows, int cols) {
    int top = rows - 4;
    int bx = 1;
    int bw = cols - 2;
    if (bw < 8) {
        return;
    }
    int border_pair = a->streaming ? P_WAIT : P_BORDER;
    wattrset(stdscr, pair_attr(a, border_pair) | A_BOLD);
    if (a->unicode) {
        mvaddstr(top, bx, "┌");
        for (int i = 1; i < bw - 1; i++) {
            mvaddstr(top, bx + i, "─");
        }
        mvaddstr(top, bx + bw - 1, "┐");
        mvaddstr(top + 1, bx, "│");
        mvaddstr(top + 1, bx + bw - 1, "│");
        mvaddstr(top + 2, bx, "└");
        for (int i = 1; i < bw - 1; i++) {
            mvaddstr(top + 2, bx + i, "─");
        }
        mvaddstr(top + 2, bx + bw - 1, "┘");
    } else {
        mvaddstr(top, bx, "+");
        for (int i = 1; i < bw - 1; i++) {
            mvaddch(top, bx + i, '-');
        }
        mvaddstr(top, bx + bw - 1, "+");
        mvaddstr(top + 1, bx, "|");
        mvaddstr(top + 1, bx + bw - 1, "|");
        mvaddstr(top + 2, bx, "+");
        for (int i = 1; i < bw - 1; i++) {
            mvaddch(top + 2, bx + i, '-');
        }
        mvaddstr(top + 2, bx + bw - 1, "+");
    }
    wattrset(stdscr, pair_attr(a, P_USER) | A_BOLD);
    mvaddstr(top + 1, bx + 2, a->unicode ? "❯" : ">");
    int tx = bx + 4;
    int maxx = bx + bw - 2;
    int avail = maxx - tx + 1;
    if (avail <= 0) {
        return;
    }
    if (a->ilen == 0) {
        wattrset(stdscr, pair_attr(a, P_DIM));
        const char *ph = a->streaming ? "queue a message" : "ask anything";
        mvaddnstr(top + 1, tx, ph, avail);
        a->cur_y = top + 1;
        a->cur_x = tx;
        a->cursor_on = 1;
        return;
    }
    int cw[MEONG_INPUT_MAX];
    size_t i;
    for (i = 0; i < a->ilen; i++) {
        wchar_t wc = a->input[i];
        int w;
        if (wc == '\t') {
            w = 4;
        } else {
            w = wcwidth(wc);
            if (w < 0) {
                w = 1;
            }
        }
        cw[i] = w;
    }
    int curpos = 0;
    for (i = 0; i < a->icur; i++) {
        curpos += cw[i];
    }
    size_t start = 0;
    while (curpos > avail - 1 && start < a->icur) {
        curpos -= cw[start];
        start++;
    }
    int x = tx;
    for (i = start; i < a->ilen && x <= maxx; i++) {
        wchar_t wc = a->input[i];
        int w = cw[i];
        if (w > 0 && x + w - 1 > maxx) {
            break;
        }
        if (wc == '\t') {
            for (int k = 0; k < w && x <= maxx; k++) {
                mvaddch(top + 1, x, ' ');
                x++;
            }
        } else {
            wattrset(stdscr, pair_attr(a, P_BASE));
            mvwaddnwstr(stdscr, top + 1, x, &wc, 1);
            x += w;
        }
    }
    a->cur_y = top + 1;
    a->cur_x = tx + curpos;
    if (a->cur_x > maxx) {
        a->cur_x = maxx;
    }
    a->cursor_on = 1;
}

static void draw_footer(App *a, int rows, int cols, int slash) {
    int y = rows - 1;
    wattrset(stdscr, pair_attr(a, P_DIM));
    const char *hint;
    if (a->help_open) {
        hint = "press any key";
    } else if (a->pet_open) {
        hint = "click card = pet | outside/key = close";
    } else if (slash) {
        if (a->unicode) {
            hint = cols < 64 ? "↑↓ | enter"
                             : "↑↓ choose | enter complete | esc close";
        } else {
            hint = cols < 64 ? "up/down | enter"
                             : "up/down choose | enter complete | esc close";
        }
    } else if (a->streaming) {
        hint = "enter queue | ctrl-c cancel";
    } else {
        hint = "enter send | ? help";
    }
    mvaddnstr(y, 1, hint, cols - 2);
    char right[160];
    right[0] = '\0';
    if (a->new_below) {
        snprintf(right, sizeof right, "%s new", sym_arrow_down(a));
    } else if (a->have_gen_stats || a->last_trimmed) {
        char n[32] = "";
        /* TODO(perf): this linearly strcmp()s a->models every frame once gen
         * stats exist. Cache context_window in App when the model or model
         * list changes; the whole draw frame is ~324k Ir (96% ncurses/libc),
         * so measure before taking it. */
        const ModelCaps *caps =
            a->model ? model_caps_for(a, a->model) : NULL;
        if (a->have_gen_stats && a->last_ptok > 0) {
            long long used = a->last_ptok + a->last_ctok;
            if (caps && caps->context_window > 0) {
                long long pct = (used * 100 + caps->context_window / 2) /
                                caps->context_window;
                if (pct < 1) {
                    snprintf(n, sizeof n, "<1%%");
                } else if (pct > 100) {
                    snprintf(n, sizeof n, "100%%");
                } else {
                    snprintf(n, sizeof n, "%lld%%", pct);
                }
            } else {
                fmt_count(a->last_ptok, n, sizeof n);
            }
        }
        if (a->last_trimmed) {
            if (n[0]) {
                snprintf(right, sizeof right, "ctx %s | trimmed", n);
            } else {
                snprintf(right, sizeof right, "context trimmed");
            }
        } else if (n[0]) {
            snprintf(right, sizeof right, "ctx %s", n);
        } else if (a->have_gen_stats && a->last_ctok > 0) {
            fmt_count(a->last_ctok, n, sizeof n);
            snprintf(right, sizeof right, "out %s", n);
        }
    }
    if (right[0]) {
        int rl = (int)strlen(right);
        int hl = (int)strlen(hint);
        if (1 + hl + 2 < cols - 1 - rl) {
            if (a->color) {
                wattrset(stdscr, pair_attr(a, a->new_below ? P_WAIT : P_DIM) |
                                      (a->new_below ? A_BOLD : 0));
            } else {
                wattrset(stdscr, A_DIM);
            }
            mvaddstr(y, cols - 1 - rl, right);
        }
    }
}

void ui_draw(App *a) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    wattrset(stdscr, pair_attr(a, P_BASE));
    for (int y = 0; y < rows; y++) {
        mvhline(y, 0, ' ', cols);
    }
    a->cursor_on = 0;
    a->chat_y0 = -1;
    a->chat_top = 0;
    a->chat_end = 0;
    a->brand_y = -1;
    a->brand_x0 = -1;
    a->brand_x1 = -1;
    a->model_y = -1;
    a->model_x0 = -1;
    a->model_x1 = -1;
    a->model_py0 = -1;
    a->model_py1 = -1;
    a->model_px0 = -1;
    a->model_px1 = -1;
    a->pet_y0 = -1;
    a->pet_y1 = -1;
    a->pet_x0 = -1;
    a->pet_x1 = -1;
    a->cmd_y0 = -1;
    if (rows < UI_MIN_ROWS || cols < UI_MIN_COLS) {
        a->help_open = 0;
        a->picker_open = 0;
        a->pet_open = 0;
        a->model_open = 0;
        const char *msg = "terminal too small";
        if (rows > 0 && cols > 0) {
            mvaddnstr(rows / 2, (cols - (int)strlen(msg)) / 2, msg, cols);
        }
        curs_set(0);
        refresh();
        return;
    }
    draw_header(a, rows, cols);
    draw_chat(a, rows, cols);
    int idx[SLASH_MAX];
    int slash_n = slash_open_list(a, idx, SLASH_MAX);
    if (slash_n > 0) {
        slash_draw(a, rows, cols, idx, slash_n);
    }
    draw_status(a, rows, cols);
    draw_input(a, rows, cols);
    draw_footer(a, rows, cols, slash_n > 0);
    if (a->picker_open) {
        picker_draw(a, rows, cols);
    } else if (a->help_open) {
        help_draw(a, rows, cols);
    } else if (a->pet_open) {
        pet_draw(a, rows, cols);
    } else if (a->model_open) {
        model_info_draw(a, rows, cols);
    }
    if (a->cursor_on) {
        wmove(stdscr, a->cur_y, a->cur_x);
        curs_set(1);
    } else {
        curs_set(0);
    }
    refresh();
}
