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
 * base 101110, surface 222522, text d9ded6, accent b6ff00, muted 747b72;
 * warn ff8700 marks the outside-cwd permission gate */
#define C_BASE 233
#define C_SURFACE 235
#define C_TEXT 253
#define C_ACCENT 154
#define C_MUTED 243
#define C_SHADOW 234
#define C_ERR 203
#define C_WARN 208

/* one quadrant filled clockwise; wall-clock phase, and the modulo below is
 * the table length, so adding a frame cannot read past the array */
#define SPIN_FRAMES(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define SPIN_HZ 5 /* 5 frames/s: one clockwise turn per 0.8 s */
/* The permission card replaces the three-row composer and grows five rows up
 * into the chat area; chat_height reserves exactly that. At UI_MIN_ROWS the
 * extra blank row does not fit above the footer, so the card falls back to
 * the compact height. */
#define PERM_CARD_H 8
#define PERM_CARD_MIN_H 7
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
        init_pair(P_PANEL_WARN, C_WARN, C_SURFACE);
        init_pair(P_SHADOW, C_SHADOW, C_SHADOW);
        init_pair(P_HOK, C_ACCENT, C_SURFACE);
        init_pair(P_HWAIT, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_CC, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_OC, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_KILO, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_BOB, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_MISTRAL, C_ACCENT, C_SURFACE);
        init_pair(P_CHIP_OTHER, C_ACCENT, C_SURFACE);
        init_pair(P_ERR, C_ERR, C_BASE);
        init_pair(P_EFFORT, C_WARN, C_BASE);
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
        init_pair(P_PANEL_WARN, COLOR_YELLOW, -1);
        init_pair(P_SHADOW, COLOR_WHITE, -1);
        init_pair(P_HOK, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_HWAIT, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_CC, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_OC, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_KILO, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_BOB, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_MISTRAL, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_CHIP_OTHER, COLOR_BLACK, COLOR_GREEN);
        init_pair(P_ERR, COLOR_RED, -1);
        init_pair(P_EFFORT, COLOR_YELLOW, -1);
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
    if (!a->unicode) {
        a->nerd = 0;
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

/* Separate from the fit check so draw_header inlines the common "fits"
 * path and only enters this frame+canary path for an over-long id. */
static int header_mid_cut(char *buf, size_t cap, const char *mid, int n,
                          int midmax, int unicode) {
    int width = midmax - 2;
    if (width < 1) {
        width = 1;
    }
    size_t eb = unicode ? 3 : 1;
    if (cap - 1 < eb) {
        return 0;
    }
    size_t maxb = cap - 1 - eb;
    size_t i = 0;
    int cols = 0;
    while (mid[i] && cols < width - 1) {
        wchar_t wc;
        int k = utf8_next(mid + i, (size_t)n - i, &wc);
        if (k <= 0) {
            break;
        }
        int w = wcwidth(wc);
        if (w < 0) {
            w = 1;
        }
        if (cols + w > width - 1 || i + (size_t)k > maxb) {
            break;
        }
        i += (size_t)k;
        cols += w;
    }
    memcpy(buf, mid, i);
    memcpy(buf + i, unicode ? "…" : "~", eb);
    buf[i + eb] = '\0';
    return cols + 1;
}

/* 0 when mid fits; else buf gets a UTF-8-safe prefix plus an ellipsis and
 * the return value is the display width written. */
int header_mid(char *buf, size_t cap, const char *mid, int midmax,
               int unicode) {
    if (!buf || cap == 0 || !mid) {
        return 0;
    }
    int n = (int)strlen(mid);
    if (n <= midmax - 2) {
        return 0;
    }
    return header_mid_cut(buf, cap, mid, n, midmax, unicode);
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
    const char *icon = sym_brand_icon(a);
    int bx = 1;
    if (icon[0]) {
        mvaddstr(0, bx, icon);
        bx++;
        mvaddstr(0, bx, " ");
        bx++;
    }
    mvaddstr(0, bx, APP_NAME);
    a->brand_y = 0;
    a->brand_x0 = 1;
    a->brand_x1 = bx + (int)strlen(APP_NAME) + 2;
    wattrset(stdscr, base | A_DIM);
    int sep_x = a->brand_x1 - 1;
    mvaddstr(0, sep_x, " |  ");

    char right[512];
    /* right/fit/midbuf put ~1.6 KB peak on the draw stack. In-place
     * right truncation was measured: -512 B frame, +16 insn and +64 B text,
     * so it was rejected. Revisit only if draw-stack size becomes a
     * constraint. */
    char fit[512];
    const char *show = right;
    int right_dim = 0;
    if (a->streaming) {
        snprintf(right, sizeof right, "%s streaming", sym_status(a));
    } else if (a->cwd && a->cwd[0]) {
        size_t n = strlen(a->cwd);
        if (n < sizeof right) {
            memcpy(right, a->cwd, n + 1);
        } else {
            size_t eb = a->unicode ? 3 : 1;
            size_t start = n - (sizeof right - 1 - eb);
            while (start < n &&
                   ((unsigned char)a->cwd[start] & 0xC0) == 0x80) {
                start++;
            }
            memcpy(right, a->unicode ? "…" : "~", eb);
            memcpy(right + eb, a->cwd + start, n - start);
            right[eb + n - start] = '\0';
        }
        right_dim = 1;
    } else {
        snprintf(right, sizeof right, "%s ready", sym_status(a));
    }
    int right_w = utf8_cols(right);

    int x = sep_x + 4;
    int midmax = cols - x - 2;
    if (right_w > 0) {
        int reserve = right_w + 1;
        if (reserve > cols / 2) {
            reserve = cols / 2;
        }
        midmax -= reserve;
    }
    /* With the icon at 36 cols the right reserve leaves 4 cells, so this
     * drops the cwd and keeps a 6-cell title chip. */
    if (midmax < 6) {
        midmax = 6;
        right_w = 0;
    }
    const char *mid = a->title[0] ? a->title : "new chat";
    int midn = utf8_cols(mid);
    char midbuf[256];
    int fitn = header_mid(midbuf, sizeof midbuf, mid, midmax, a->unicode);
    if (fitn > 0) {
        mid = midbuf;
        midn = fitn;
    }
    wattrset(stdscr, base | (a->title[0] ? A_BOLD : A_DIM));
    mvaddstr(0, x, mid);
    int mend = x + midn;

    if (right_w > 0) {
        int avail = cols - 2 - mend;
        if (right_w > avail) {
            int keep = avail;
            if (keep > 1) {
                const char *ell = a->unicode ? "…" : "~";
                size_t eb = a->unicode ? 3 : 1;
                size_t ts = utf8_tail(right, keep - 1);
                size_t sl = strlen(right + ts);
                size_t room = sizeof fit - 1 - eb;
                if (sl > room) {
                    sl = room;
                    while (sl > 0 &&
                           ((unsigned char)right[ts + sl] & 0xC0) == 0x80) {
                        sl--;
                    }
                }
                memcpy(fit, ell, eb);
                memcpy(fit + eb, right + ts, sl);
                fit[eb + sl] = '\0';
                show = fit;
                right_w = utf8_cols(show);
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
    if (a->tool_pending) {
        /* The permission card owns the bottom block and carries its hint. */
        return 0;
    }
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
    int card_h = rows < 11 ? PERM_CARD_MIN_H : PERM_CARD_H;
    int extra = a->tool_pending ? card_h - 3 : 0;
    int h = rows - 6 - status_rows(a) - extra;
    return h > 1 ? h : 1;
}

static void draw_status(App *a, int rows, int cols) {
    int n = status_rows(a);
    if (n == 0) {
        return;
    }
    int y = rows - 5 - n;
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
            draw_slice(a, y, x, a->queued[0], 0, (int)strlen(a->queued[0]),
                       maxx, a->color ? pair_attr(a, P_DIM) : 0, 0, 0, 0,
                       &endx);
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

static const ToolCall *perm_pending_call(const App *a) {
    if (a->tool_msg_idx < 0 || a->tool_msg_idx >= a->nmsg) {
        return NULL;
    }
    const Msg *m = &a->msgs[a->tool_msg_idx];
    if (a->tool_call_pos < 0 || a->tool_call_pos >= m->ncalls) {
        return NULL;
    }
    return &m->calls[a->tool_call_pos];
}

static size_t perm_args_text(const App *a, const ToolCall *call, char *out,
                             size_t cap) {
    const char *s = call && call->args ? call->args : a->perm_ask.path;
    size_t n = call && call->args ? call->args_len : strlen(a->perm_ask.path);
    size_t o = 0;
    size_t i = 0;
    for (; i < n && o + 4 < cap; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            ch = ' ';
        } else if (ch < 32) {
            continue;
        }
        out[o++] = (char)ch;
    }
    if (i < n) {
        out[o++] = '.';
        out[o++] = '.';
        out[o++] = '.';
    }
    out[o] = '\0';
    if (o == 0) {
        out[0] = '{';
        out[1] = '}';
        out[2] = '\0';
        o = 2;
    }
    return o;
}

static void perm_button(const App *a, int y, int x, int bw,
                        const char *label, int sel) {
    if (sel) {
        attr_t sa = a->color ? pair_attr(a, P_SEL) : (attr_t)A_REVERSE;
        wattrset(stdscr, sa);
        mvhline(y, x, ' ', bw);
        wattrset(stdscr, sa | A_BOLD);
    } else {
        wattrset(stdscr, pair_attr(a, P_PANEL_ACCENT) | A_BOLD);
    }
    mvaddstr(y, x, "[ ");
    mvaddstr(y, x + 2, label);
    mvaddstr(y, x + bw - 2, " ]");
}

static __attribute__((noinline, cold)) void perm_draw(App *a, int rows,
                                                      int cols) {
    int w = cols - 2;
    if (w < 30) {
        return;
    }
    int px = 1;
    int card_h = rows < 11 ? PERM_CARD_MIN_H : PERM_CARD_H;
    int pad = card_h - PERM_CARD_MIN_H;
    int py = rows - 3 - card_h + 1;
    if (py < 1) {
        py = 1;
    }
    const ToolCall *call = perm_pending_call(a);
    const char *name = call && call->name && *call->name
                           ? call->name
                           : (a->perm_ask.tool[0] ? a->perm_ask.tool
                                                  : "tool");
    /* 512 covers every realistic width; the frame is cold, so shrinking it
     * was measured and rejected. The 2026-11-20 audit measured 370 -> 468
     * insn and 720 -> 848 B for the three-class label/hint, with x19-x28
     * already full: no register headroom, so this is a rejection, not a
     * TODO queue item. */
    char args[512];

    panel_frame(a, px, py, w, card_h, NULL, 0);

    const char *label = " permission ";
    int tx = px + 3;
    wattrset(stdscr, pair_attr(a, P_PANEL_ACCENT) | A_BOLD);
    const char *shield = sym_perm_shield(a);
    if (shield[0]) {
        mvaddstr(py, tx, shield);
        tx += 2;
    }
    mvaddstr(py, tx, label);
    tx += (int)strlen(label) - 1;

    const char *wlabel = "outside cwd";
    if (a->perm_ask.action == TOOL_ACT_MUTATE) {
        wlabel = "write";
    } else if (a->perm_ask.action == TOOL_ACT_EXEC) {
        wlabel = "exec";
    }
    char where[24];
    snprintf(where, sizeof where, " %s ", wlabel);
    int where_w = (int)strlen(where);
    int where_x = px + w - 3 - where_w;
    if (where_x >= tx + 2) {
        wattrset(stdscr, a->color ? pair_attr(a, P_PANEL_WARN)
                                  : (attr_t)A_BOLD);
        mvaddstr(py, where_x, where);
    }

    int x0 = px + 2;
    int maxx = px + w - 2;
    int cy = py + pad;
    int y = cy + 1;
    wattrset(stdscr, pair_attr(a, P_PANEL_ACCENT) | A_BOLD);
    mvaddstr(y, x0, sym_tool_ask(a));
    mvaddnstr(y, x0 + 2, name, maxx - (x0 + 2) + 1);
    int textw = maxx - (x0 + 2) + 1;
    size_t args_cap = sizeof args;
    if (args_cap > (size_t)textw + 4) {
        args_cap = (size_t)textw + 4;
    }
    int args_len;
    if (a->perm_ask.kind != TOOL_ASK_PATH && a->perm_ask.text[0]) {
        size_t n = a->perm_ask.text_len;
        if (n >= args_cap) {
            n = args_cap - 1;
        }
        memcpy(args, a->perm_ask.text, n);
        args[n] = '\0';
        if (a->perm_ask.text_truncated && n + 3 < args_cap) {
            memcpy(args + n, "...", 4);
            n += 3;
        }
        args_len = (int)n;
    } else {
        args_len = (int)perm_args_text(a, call, args, args_cap);
    }
    draw_slice(a, y + 1, x0 + 2, args, 0, args_len, maxx,
               pair_attr(a, P_PANEL_DIM), 0, 0, 0, NULL);

    int avail = maxx - x0 + 1;
    int nbtn = a->perm_ask.action == TOOL_ACT_READ ? 3 : 2;
    const char *labels[3] = {"Allow", "Reject", "Always"};
    int gap = 1;
    if (avail >= 52) {
        labels[2] = "Always allow this session";
        gap = 2;
    }
    int by = cy + 4;
    int bx = x0;
    a->perm_btn_y = by;
    for (int i = 0; i < nbtn; i++) {
        int bw = (int)strlen(labels[i]) + 4;
        a->perm_btn_x0[i] = bx;
        a->perm_btn_x1[i] = bx + bw - 1;
        perm_button(a, by, bx, bw, labels[i], a->perm_sel == i);
        bx += bw + gap;
    }
    for (int i = nbtn; i < 3; i++) {
        a->perm_btn_x0[i] = -1;
        a->perm_btn_x1[i] = -1;
    }

    const char *hint;
    char hintbuf[80];
    if (nbtn != 3) {
        snprintf(hintbuf, sizeof hintbuf, "%s%s", wlabel,
                 a->unicode ? " · arrows choose · enter"
                            : " | arrows choose | enter");
        hint = hintbuf;
    } else if (a->unicode) {
        if (avail >= 59) {
            hint = "outside cwd · arrows choose · enter · always = this session";
        } else if (avail >= 43) {
            hint = "outside cwd · enter · always = this session";
        } else {
            hint = "always = this session";
        }
    } else {
        if (avail >= 59) {
            hint = "outside cwd | arrows choose | enter | always = this session";
        } else if (avail >= 43) {
            hint = "outside cwd | enter | always = this session";
        } else {
            hint = "always = session";
        }
    }
    wattrset(stdscr, pair_attr(a, P_PANEL_DIM));
    mvaddnstr(cy + 5, x0, hint, avail + 8);
    a->cursor_on = 0;
}

static void draw_input(App *a, int rows, int cols) {
    int top = rows - 5;
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
        const char *ph =
            a->streaming ? "queue a message" : "ask anything";
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
    int y = rows - 2;
    wattrset(stdscr, pair_attr(a, P_DIM));
    const char *hint;
    if (a->tool_pending) {
        if (a->unicode) {
            hint = cols < 64 ? "←→ | enter | y/n/a"
                             : "←→ choose | enter | y/n/a";
        } else {
            hint = cols < 64 ? "left/right | enter | y/n/a"
                             : "left/right choose | enter | y/n/a";
        }
    } else if (a->error_open) {
        hint = a->unicode ? "↑↓ choose | enter | esc close"
                          : "up/down choose | enter | esc close";
    } else if (a->help_open) {
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
    } else if (a->effort_open) {
        if (a->unicode) {
            hint = cols < 64 ? "↑↓ | enter | esc"
                             : "↑↓ choose | enter apply | esc keep";
        } else {
            hint = cols < 64 ? "up/down | enter | esc"
                             : "up/down choose | enter apply | esc keep";
        }
    } else if (a->streaming) {
        hint = "enter queue | ctrl-c cancel";
    } else {
        hint = "enter send | ? help";
    }
    mvaddnstr(y, 1, hint, cols - 2);
    char right[160];
    char n[32];
    size_t pos = 0; /* worst case ~64 B, so appends stay inside right[160] */
    size_t cache_cut = 0;
    int had_cache = 0;
    right[0] = '\0';
    n[0] = '\0';
    if (a->new_below) {
        pos = (size_t)snprintf(right, sizeof right, "%s new",
                               sym_arrow_down(a));
    } else if (a->have_gen_stats || a->last_trimmed) {
        if (a->model_ctx == MODEL_CTX_UNSET) {
            const ModelCaps *caps =
                a->model ? model_caps_for(a, a->model) : NULL;
            a->model_ctx = caps ? caps->context_window : -1;
        }
        if (a->have_gen_stats && a->last_ptok > 0) {
            long long used = a->last_ptok + a->last_ctok;
            if (a->model_ctx > 0) {
                long long pct =
                    (used * 100 + a->model_ctx / 2) / a->model_ctx;
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
        if (n[0]) {
            pos = (size_t)snprintf(right, sizeof right, "ctx %s", n);
        } else if (a->last_trimmed) {
            pos = (size_t)snprintf(right, sizeof right, "context trimmed");
        } else if (a->have_gen_stats && a->last_ctok > 0) {
            fmt_count(a->last_ctok, n, sizeof n);
            pos = (size_t)snprintf(right, sizeof right, "out %s", n);
        }
        if (a->have_gen_stats && a->last_ptok > 0) {
            had_cache = 1;
            cache_cut = pos;
            if (a->last_has_cached) {
                long long pct =
                    (a->last_cached * 100 + a->last_ptok / 2) /
                    a->last_ptok;
                if (pct < 0) {
                    pct = 0;
                }
                if (pct > 100) {
                    pct = 100;
                }
                pos += (size_t)snprintf(right + pos, sizeof right - pos,
                                        "%scache %lld%%",
                                        pos ? " | " : "", pct);
            } else {
                pos += (size_t)snprintf(right + pos, sizeof right - pos,
                                        "%scache -", pos ? " | " : "");
            }
        }
        if (a->last_trimmed && n[0]) {
            pos += (size_t)snprintf(right + pos, sizeof right - pos,
                                    " | trimmed");
        }
    }
    if (right[0]) {
        /* rejected: strlen(right) here measured 0x1c0 frame / 806 ins */
        int rl = (int)pos;
        int hl = (int)strlen(hint);
        if (!(1 + hl + 2 < cols - 1 - rl) && had_cache) {
            /* Narrow footer: drop the cache segment before the context. */
            right[cache_cut] = '\0';
            pos = cache_cut;
            if (a->last_trimmed && n[0]) {
                pos += (size_t)snprintf(right + pos, sizeof right - pos,
                                        " | trimmed");
            }
            rl = (int)pos;
        }
        if (right[0] && 1 + hl + 2 < cols - 1 - rl) {
            if (a->color) {
                wattrset(stdscr, pair_attr(a, a->new_below ? P_WAIT : P_DIM) |
                                      (a->new_below ? A_BOLD : 0));
            } else {
                wattrset(stdscr, A_DIM);
            }
            mvaddstr(y, cols - 1 - rl, right);
        }
    }
    /* Second footer line: the model id, then the orange effort label. */
    if (a->model && a->model[0]) {
        int my = rows - 1;
        const char *eff = effort_str(a);
        if (!eff[0]) {
            eff = "default";
        }
        int elen = (int)strlen(eff);
        int gap = 2;
        int avail = cols - 2 - gap - elen;
        if (avail < 1) {
            avail = 1;
        }
        int mw = (int)strlen(a->model);
        if (mw > avail) {
            mw = avail;
        }
        wattrset(stdscr, pair_attr(a, P_ACCENT));
        mvaddnstr(my, 1, a->model, mw);
        wattrset(stdscr, pair_attr(a, P_EFFORT));
        mvaddstr(my, 1 + mw + gap, eff);
        a->model_y = my;
        a->model_x0 = 1;
        a->model_x1 = 1 + mw;
        a->effort_y = my;
        a->effort_x0 = 1 + mw + gap;
        a->effort_x1 = a->effort_x0 + elen;
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
    a->effort_y = -1;
    a->effort_x0 = -1;
    a->effort_x1 = -1;
    a->model_py0 = -1;
    a->model_py1 = -1;
    a->model_px0 = -1;
    a->model_px1 = -1;
    a->error_y0 = -1;
    a->error_y1 = -1;
    a->error_x0 = -1;
    a->error_x1 = -1;
    a->error_retry_y = -1;
    a->error_close_y = -1;
    a->pet_y0 = -1;
    a->pet_y1 = -1;
    a->pet_x0 = -1;
    a->pet_x1 = -1;
    a->cmd_y0 = -1;
    if (rows < UI_MIN_ROWS || cols < UI_MIN_COLS) {
        a->help_open = 0;
        a->picker_open = 0;
        a->effort_open = 0;
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
    int slash_n = slash_open_list_at(a, rows, cols, idx, SLASH_MAX);
    if (slash_n > 0) {
        slash_draw(a, rows, cols, idx, slash_n);
    }
    draw_status(a, rows, cols);
    if (a->tool_pending) {
        perm_draw(a, rows, cols);
    } else {
        draw_input(a, rows, cols);
    }
    draw_footer(a, rows, cols, slash_n > 0);
    if (a->picker_open) {
        picker_draw(a, rows, cols);
    } else if (a->effort_open) {
        effort_picker_draw(a, rows, cols);
    } else if (a->help_open) {
        help_draw(a, rows, cols);
    } else if (a->pet_open) {
        pet_draw(a, rows, cols);
    } else if (a->model_open) {
        model_info_draw(a, rows, cols);
    }
    if (a->error_open) {
        error_draw(a, rows, cols);
    }
    if (a->cursor_on) {
        wmove(stdscr, a->cur_y, a->cur_x);
        curs_set(1);
    } else {
        curs_set(0);
    }
    refresh();
}
