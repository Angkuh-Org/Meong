#include "ui/ui.h"

#include <stdlib.h>
#include <string.h>

enum {
    RL_HEAD_E,
    RL_TEXT,
    RL_CODE,
    RL_FENCE,
    RL_REASON,
    RL_REASON_HEAD,
    RL_NOTE,
    RL_GAP,
};


struct RLine {
    int type;
    int msg;
    int off;
    int len;
    unsigned char cont;
    unsigned char cursor;
    unsigned char head;
};

static void line_push(App *a, int type, int msg, int off, int len, int cont) {
    if (a->nlines == a->linecap) {
        a->linecap = a->linecap ? a->linecap * 2 : 128;
        a->lines = xrealloc(a->lines, (size_t)a->linecap * sizeof *a->lines);
    }
    RLine *l = &a->lines[a->nlines++];
    l->type = type;
    l->msg = msg;
    l->off = off;
    l->len = len;
    l->cont = (unsigned char)(cont != 0);
    l->cursor = 0;
    l->head = 0;
}

static int line_is_fence(const char *s, size_t len) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    return i + 3 <= len && memcmp(s + i, "```", 3) == 0;
}

static void mark_tail(Msg *m, size_t off, int row, int cont, size_t para,
                      int code) {
    m->lay_off = off;
    m->lay_para = para;
    m->lay_row = row;
    m->lay_cont = cont;
    m->lay_code = code;
    m->lay_valid = 1;
}

static void wrap_run(App *a, int mi, const char *s, size_t len, size_t base,
                     int type, int width, size_t from, int cont_start,
                     int code, int mark) {
    Msg *m = &a->msgs[mi];
    if (width < 8) {
        width = 8;
    }
    if (len == 0) {
        line_push(a, type, mi, (int)base, 0, cont_start);
        if (mark) {
            mark_tail(m, base, a->nlines - 1, cont_start, base, code);
        }
        return;
    }
    size_t pos = from;
    int cont = cont_start;
    while (pos < len) {
        size_t start = pos;
        size_t i = pos;
        int col = 0;
        size_t last_space = (size_t)-1;
        while (i < len) {
            unsigned char ch = (unsigned char)s[i];
            if (ch >= 32 && ch < 127) {
                int room = width - col;
                size_t j = i;
                while (j < len && room > 0) {
                    unsigned char b = (unsigned char)s[j];
                    if (b < 32 || b >= 127) {
                        break;
                    }
                    if (b == ' ' && col + (int)(j - i) + 1 < width) {
                        last_space = j;
                    }
                    j++;
                    room--;
                }
                col += (int)(j - i);
                i = j;
                if (i < len && room == 0) {
                    break;
                }
                continue;
            }
            wchar_t wc;
            int bytes = utf8_next(s + i, len - i, &wc);
            if (bytes <= 0) {
                break;
            }
            if (wc == '\r') {
                i += (size_t)bytes;
                continue;
            }
            int cw;
            if (wc == '\t') {
                cw = 4 - (col % 4);
            } else {
                cw = wcwidth(wc);
                if (cw < 0) {
                    cw = 1;
                }
            }
            if (cw > 0 && col + cw > width) {
                break;
            }
            if (wc == ' ' && col + cw < width) {
                last_space = i;
            }
            col += cw;
            i += (size_t)bytes;
        }
        size_t end = i;
        size_t next = i;
        if (i < len) {
            if (last_space != (size_t)-1 && last_space > start) {
                end = last_space;
                next = last_space + 1;
            }
        }
        if (end <= start) {
            wchar_t wc;
            int bytes = utf8_next(s + i, len - i, &wc);
            next = i + (bytes > 0 ? (size_t)bytes : 1);
            if (next > len) {
                next = len;
            }
            end = next;
        }
        while (next < len && s[next] == ' ') {
            next++;
        }
        line_push(a, type, mi, (int)(base + start), (int)(end - start), cont);
        if (mark) {
            mark_tail(m, base + start, a->nlines - 1, cont, base, code);
        }
        cont = 1;
        pos = next;
    }
}

static void layout_message(App *a, int mi, int incremental) {
    Msg *m = &a->msgs[mi];
    if (m->lay_cursor > 0) {
        int cr = m->lay_cursor - 1;
        if (cr < a->nlines && a->lines[cr].msg == mi) {
            a->lines[cr].cursor = 0;
        }
        m->lay_cursor = 0;
    }
    int inc = incremental && m->lay_valid;
    size_t inc_off = m->lay_off;
    size_t inc_para = m->lay_para;
    int inc_row = m->lay_row;
    int inc_cont = m->lay_cont;
    int inc_code = m->lay_code;
    int inc_text_start = m->lay_text_start;
    m->lay_valid = 0;

    if (m->role == ROLE_NOTE) {
        if (m->len == 0) {
            line_push(a, RL_NOTE, mi, 0, 0, 0);
        } else {
            wrap_run(a, mi, m->text, m->len, 0, RL_NOTE, a->layout_width,
                     0, 0, 0, 0);
        }
        line_push(a, RL_GAP, mi, 0, 0, 0);
        return;
    }
    if (m->role == ROLE_ERROR) {
        line_push(a, RL_HEAD_E, mi, 0, 0, 0);
        if (m->len == 0) {
            line_push(a, RL_TEXT, mi, 0, 0, 0);
        } else {
            wrap_run(a, mi, m->text, m->len, 0, RL_TEXT, a->layout_width,
                     0, 0, 0, 0);
        }
        line_push(a, RL_GAP, mi, 0, 0, 0);
        return;
    }

    int first = -1;
    int text_start;
    int in_code = 0;
    size_t pos = 0;
    if (!inc) {
        if (m->role == ROLE_ASSISTANT && m->rlen > 0) {
            line_push(a, RL_REASON_HEAD, mi, 0, 0, 0);
            first = a->nlines - 1;
            if (!m->reason_collapsed) {
                wrap_run(a, mi, m->reason, m->rlen, 0, RL_REASON,
                         a->layout_width, 0, 0, 0, 0);
                if (m->len > 0) {
                    line_push(a, RL_GAP, mi, 0, 0, 0);
                }
            }
        }
        text_start = a->nlines;
    } else {
        text_start = inc_text_start;
        a->nlines = inc_row;
        if (m->role == ROLE_ASSISTANT && m->rlen > 0 &&
            !m->reason_collapsed && m->len > 0 && inc_row == text_start &&
            (text_start == 0 ||
             a->lines[text_start - 1].type != RL_GAP)) {
            line_push(a, RL_GAP, mi, 0, 0, 0);
            text_start = a->nlines;
            m->lay_text_start = text_start;
        }
        pos = inc_para;
        in_code = inc_code;
    }

    if (m->len > 0) {
        while (pos < m->len) {
            size_t scan = pos;
            if (inc && pos == inc_para && inc_off > scan) {
                scan = inc_off;
            }
            size_t nl = scan;
            while (nl < m->len && m->text[nl] != '\n') {
                nl++;
            }
            size_t llen = nl - pos;
            int line_code = in_code;
            if (line_is_fence(m->text + pos, llen)) {
                line_push(a, RL_FENCE, mi, (int)pos, (int)llen, 0);
                mark_tail(m, pos, a->nlines - 1, 0, pos, line_code);
                in_code = !in_code;
            } else if (llen == 0) {
                line_push(a, in_code ? RL_CODE : RL_TEXT, mi, (int)pos, 0, 0);
                mark_tail(m, pos, a->nlines - 1, 0, pos, line_code);
            } else {
                size_t from = 0;
                int cont = 0;
                if (inc && pos == inc_para && inc_off > pos) {
                    from = inc_off - pos;
                    cont = inc_cont;
                }
                wrap_run(a, mi, m->text + pos, llen, pos,
                         in_code ? RL_CODE : RL_TEXT, a->layout_width,
                         from, cont, line_code, 1);
            }
            if (nl == m->len) {
                break;
            }
            pos = nl + 1;
            mark_tail(m, pos, a->nlines, 0, pos, in_code);
        }
    } else {
        line_push(a, RL_TEXT, mi, 0, 0, 0);
        mark_tail(m, 0, a->nlines - 1, 0, 0, 0);
    }

    if (inc) {
        if (m->lay_text_start >= 0 && m->lay_text_start < a->nlines &&
            a->lines[m->lay_text_start].msg == mi) {
            a->lines[m->lay_text_start].head = 1;
        }
    } else {
        if (first < 0) {
            first = text_start;
        }
        if (first >= 0 && first < a->nlines) {
            a->lines[first].head = 1;
        }
        if (m->role == ROLE_ASSISTANT && m->rlen > 0 && m->len > 0 &&
            text_start < a->nlines) {
            a->lines[text_start].head = 1;
        }
        m->lay_text_start = text_start;
    }
    line_push(a, RL_GAP, mi, 0, 0, 0);
    if (m->streaming) {
        for (int i = a->nlines - 1; i >= 0; i--) {
            if (a->lines[i].msg != mi) {
                break;
            }
            if (a->lines[i].type != RL_GAP) {
                a->lines[i].cursor = 1;
                m->lay_cursor = i + 1;
                break;
            }
        }
    }
}

/*
 * Cursor invariant: while lay_valid, m->text is append-only and the bytes
 * before lay_off do not change.  msg_set, trim_trailing_ws, msg_append_reason,
 * reason_toggle and a width change clear lay_valid; msg_append may keep it.
 * A layout_from past nmsg means messages were removed, so rebuild from zero.
 */
static void layout_build(App *a) {
    int keep = a->layout_from;
    if (keep < 0) {
        keep = 0;
    }
    if (keep > a->nmsg) {
        keep = 0;
    }
    int inc = 0;
    if (keep >= 0 && keep < a->nmsg && keep == a->nmsg - 1) {
        Msg *m = &a->msgs[keep];
        const char *p = m->text ? m->text : "";
        if (m->lay_valid && m->lay_off <= m->len &&
            m->lay_para <= m->lay_off &&
            m->lay_row >= m->lay_text_start &&
            m->lay_row <= a->nlines &&
            (m->lay_para == 0 || p[m->lay_para - 1] == '\n')) {
            inc = 1;
        }
    }
    if (inc) {
        while (a->nlines > 0 && a->lines[a->nlines - 1].msg > keep) {
            a->nlines--;
        }
        layout_message(a, keep, 1);
    } else {
        while (a->nlines > 0 && a->lines[a->nlines - 1].msg >= keep) {
            a->nlines--;
        }
        for (int i = keep; i < a->nmsg; i++) {
            layout_message(a, i, 0);
        }
    }
    a->layout_from = a->nmsg;
    a->layout_dirty = 0;
    a->last_layout = now_sec();
}

static void reason_toggle(App *a, int mi) {
    Msg *m = &a->msgs[mi];
    if (m->rlen == 0) {
        return;
    }
    m->reason_collapsed = !m->reason_collapsed;
    m->reason_user_set = 1;
    m->lay_valid = 0;
    a->layout_from = mi;
    a->layout_dirty = 1;
    a->dirty = 1;
}

void chat_toggle_reason(App *a) {
    for (int i = a->nmsg - 1; i >= 0; i--) {
        if (a->msgs[i].role == ROLE_ASSISTANT && a->msgs[i].rlen > 0) {
            reason_toggle(a, i);
            return;
        }
    }
}

int chat_click(App *a, int y) {
    if (a->chat_y0 < 0 || y < a->chat_y0 || y >= a->chat_end) {
        return 0;
    }
    int li = a->chat_top + (y - a->chat_y0);
    if (li < 0 || li >= a->nlines) {
        return 0;
    }
    RLine *l = &a->lines[li];
    if (l->type != RL_REASON_HEAD && l->type != RL_REASON) {
        return 0;
    }
    if (l->msg < 0 || l->msg >= a->nmsg) {
        return 0;
    }
    reason_toggle(a, l->msg);
    return 1;
}

int draw_slice(int y, int x, const char *s, int off, int len, int maxx,
                      int pair, int attr, int *endx) {
    int cx = x;
    int col = 0;
    int i = off;
    int end = off + len;
    wattrset(stdscr, (attr_t)pair | (attr_t)attr);
    while (i < end && cx <= maxx) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 32 && ch < 127) {
            int j = i;
            while (j < end && (unsigned char)s[j] >= 32 &&
                   (unsigned char)s[j] < 127) {
                j++;
            }
            int run = j - i;
            if (cx + run - 1 > maxx) {
                run = maxx - cx + 1;
            }
            if (run > 0) {
                mvaddnstr(y, cx, s + i, run);
                cx += run;
                col += run;
                i += run;
            }
            continue;
        }
        wchar_t wc;
        int n = utf8_next(s + i, (size_t)(end - i), &wc);
        if (n <= 0) {
            break;
        }
        if (wc == '\r') {
            i += n;
            continue;
        }
        if (wc == '\t') {
            int adv = 4 - (col % 4);
            while (adv-- > 0 && cx <= maxx) {
                mvaddch(y, cx, ' ');
                cx++;
                col++;
            }
            i += n;
            continue;
        }
        if (wc < 32) {
            i += n;
            continue;
        }
        int w = wcwidth(wc);
        if (w < 0) {
            w = 1;
        }
        if (w > 0 && cx + w - 1 > maxx) {
            break;
        }
        if (i + n <= end) {
            char tmp[8];
            memcpy(tmp, s + i, (size_t)n);
            tmp[n] = '\0';
            mvaddstr(y, cx, tmp);
        }
        cx += w;
        col += w;
        i += n;
    }
    if (endx) {
        *endx = cx;
    }
    return cx;
}

void draw_chat(App *a, int rows, int cols) {
    int y0 = 1;
    int h = chat_height(a, rows);
    a->chat_y0 = -1;
    a->chat_top = 0;
    a->chat_end = y0 + h;
    int text_width = cols - 6;
    if (text_width < 10) {
        text_width = 10;
    }
    int need_layout = a->layout_dirty || a->layout_width != text_width;
    if (need_layout) {
        if (a->streaming && now_sec() - a->last_layout < 0.09) {
            need_layout = 0;
        }
    }
    if (need_layout) {
        if (a->layout_width != text_width) {
            a->layout_width = text_width;
            a->layout_from = 0;
            for (int i = 0; i < a->nmsg; i++) {
                a->msgs[i].lay_valid = 0;
            }
        }
        layout_build(a);
    }
    if (a->follow) {
        a->top = a->nlines > h ? a->nlines - h : 0;
    } else if (a->top > a->nlines - h) {
        a->top = a->nlines > h ? a->nlines - h : 0;
    }
    if (a->top < 0) {
        a->top = 0;
    }
    if (a->nmsg == 0 && !a->streaming) {
        draw_splash(a, y0, h, cols);
        return;
    }
    int y = y0;
    if (a->follow && a->nlines < h) {
        y = y0 + (h - a->nlines);
    }
    a->chat_y0 = y;
    a->chat_top = a->top;
    int maxx = cols - 3;
    for (int i = 0; i < h; i++) {
        int li = a->top + i;
        if (li < 0 || li >= a->nlines) {
            break;
        }
        RLine *l = &a->lines[li];
        int x = 4;
        int endx = x;
        if (l->type == RL_GAP) {
            Msg *gm = &a->msgs[l->msg];
            int last_gap =
                li + 1 >= a->nlines || a->lines[li + 1].msg != l->msg;
            if (last_gap && gm->role == ROLE_ASSISTANT &&
                (gm->stopped || gm->has_stats)) {
                char st[96];
                char stat[64];
                stat[0] = '\0';
                if (gm->has_stats) {
                    char el[32];
                    fmt_elapsed(gm->elapsed, el, sizeof el);
                    if (gm->ctok > 0) {
                        snprintf(stat, sizeof stat, "%ld tok · %s",
                                 gm->ctok, el);
                    } else {
                        snprintf(stat, sizeof stat, "%s", el);
                    }
                }
                if (gm->stopped && stat[0]) {
                    snprintf(st, sizeof st, "stopped · %s", stat);
                } else if (gm->stopped) {
                    snprintf(st, sizeof st, "stopped");
                } else {
                    snprintf(st, sizeof st, "%s", stat);
                }
                int sl = (int)strlen(st);
                if (st[0] && sl < cols - 4) {
                    wattrset(stdscr, pair_attr(a, P_DIM));
                    mvaddstr(y, cols - 3 - sl, st);
                }
            }
            y++;
            continue;
        }
        if (l->type == RL_NOTE) {
            wattrset(stdscr, pair_attr(a, P_NOTE));
            if (!l->cont) {
                mvaddstr(y, 2, sym_dot(a));
            }
            draw_slice(y, x, a->msgs[l->msg].text, l->off, l->len, maxx,
                       a->color ? pair_attr(a, P_NOTE) : 0, 0, &endx);
            y++;
            continue;
        }
        Msg *m = &a->msgs[l->msg];
        int gut_pair = m->role == ROLE_USER
                           ? P_USER
                           : (m->role == ROLE_ERROR ? P_ERROR : P_AI);
        if (l->head) {
            const char *s;
            int hp = m->role == ROLE_USER ? P_USER : P_AI;
            if (l->type == RL_REASON_HEAD) {
                s = sym_think(a);
            } else {
                s = m->role == ROLE_USER ? sym_user(a) : sym_ai(a);
            }
            wattrset(stdscr, pair_attr(a, hp) | A_BOLD);
            mvaddstr(y, 2, s);
        }
        if (l->type == RL_HEAD_E) {
            wattrset(stdscr, pair_attr(a, P_ERROR) | A_BOLD);
            mvaddstr(y, 2, sym_err(a));
            mvaddstr(y, 4, "error");
            y++;
            continue;
        }
        const char *gut = sym_gutter(a);
        const char *text = m->text;
        int off = l->off;
        int len = l->len;
        int text_pair = m->role == ROLE_USER ? P_USER : P_BASE;
        int text_attr = 0;
        if (l->type == RL_REASON_HEAD) {
            int tx = 4;
            wattrset(stdscr, pair_attr(a, P_DIM));
            mvaddstr(y, tx, "thinking");
            mvaddstr(y, tx + (int)sizeof "thinking",
                     m->reason_collapsed ? sym_fold_closed(a)
                                         : sym_fold_open(a));
            y++;
            continue;
        }
        if (l->type == RL_REASON) {
            gut = sym_reason_gutter(a);
            text = m->reason;
            text_pair = P_DIM;
        } else if (l->type == RL_CODE) {
            text_pair = P_CODE;
        } else if (l->type == RL_FENCE) {
            text_pair = P_FENCE;
        }
        if (l->type == RL_TEXT && m->role == ROLE_ERROR) {
            gut_pair = P_ERROR;
        }
        int show_gutter =
            !l->head && !(m->role == ROLE_ASSISTANT && l->type != RL_REASON);
        if (show_gutter) {
            wattrset(stdscr, pair_attr(a, gut_pair) |
                                  (m->role == ROLE_ERROR ? 0 : A_DIM));
            mvaddstr(y, 2, gut);
        }
        draw_slice(y, x, text, off, len, maxx,
                   a->color ? pair_attr(a, text_pair) : 0, text_attr, &endx);
        if (l->cursor && m->streaming && a->follow) {
            int blink = ((long long)(now_sec() * 2)) & 1;
            if (blink && endx <= maxx - 1) {
                wattrset(stdscr, pair_attr(a, P_ACCENT) | A_BOLD);
                mvaddstr(y, endx, sym_cursor(a));
            }
        }
        y++;
    }
    draw_scrollbar(a, y0, h, rows, cols);
}

