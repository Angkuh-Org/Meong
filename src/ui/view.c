#include "ui/ui.h"
#include "ui/md.h"

#include <stdlib.h>
#include <string.h>

enum {
    RL_TEXT,
    RL_CODE,
    RL_FENCE,
    RL_REASON,
    RL_REASON_HEAD,
    RL_NOTE,
    RL_GAP,
    RL_TOOL_HEAD,
    RL_TOOL_TOP,
    RL_TOOL_BODY,
    RL_TOOL_BOT,
    RL_RULE,
    RL_CBOX_TOP,
    RL_CBOX_BOT,
};


struct RLine {
    int type;
    int msg;
    int off;
    int len;
    unsigned char cont;
    unsigned char cursor;
    unsigned char head;
    unsigned char close;
    unsigned char hang;
    unsigned char style;
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
    l->close = 0;
    l->hang = 0;
    l->style = 0;
}

static int line_is_fence(const char *s, size_t len) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    return i + 3 <= len && memcmp(s + i, "```", 3) == 0;
}

static void mark_tail(Msg *m, size_t off, int row, int cont, size_t para,
                      int code, unsigned style) {
    m->lay_off = off;
    m->lay_para = para;
    m->lay_row = row;
    m->lay_cont = cont;
    m->lay_code = code;
    m->lay_style = (unsigned char)style;
    m->lay_valid = 1;
}

static void wrap_run(App *a, int mi, const char *s, size_t len, size_t base,
                     int type, int width, size_t from, int cont_start,
                     int code, int mark, int hang, unsigned style0) {
    Msg *m = &a->msgs[mi];
    if (width < 8) {
        width = 8;
    }
    if (len == 0) {
        line_push(a, type, mi, (int)base, 0, cont_start);
        if (mark) {
            mark_tail(m, base, a->nlines - 1, cont_start, base, code, style0);
        }
        return;
    }
    size_t pos = from;
    int cont = cont_start;
    unsigned mask = style0;
    while (pos < len) {
        size_t start = pos;
        size_t i = pos;
        int col = 0;
        int row_width = width - (cont ? hang : 0);
        if (row_width < 8) {
            row_width = 8;
        }
        size_t last_space = (size_t)-1;
        unsigned row_style = mask;
        while (i < len) {
            unsigned char ch = (unsigned char)s[i];
            if (ch == MD_CMD) {
                if (i + 1 < len) {
                    unsigned char cmd = (unsigned char)s[i + 1];
                    if (cmd & MD_META) {
                        i += 2;
                        if (i < len) {
                            i++;
                        }
                    } else {
                        mask = cmd;
                        i += 2;
                    }
                } else {
                    i++;
                }
                continue;
            }
            if (ch >= 32 && ch < 127) {
                int room = row_width - col;
                size_t j = i;
                while (j < len && room > 0) {
                    unsigned char b = (unsigned char)s[j];
                    if (b < 32 || b >= 127) {
                        break;
                    }
                    if (b == ' ' && col + (int)(j - i) + 1 < row_width) {
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
            if (cw > 0 && col + cw > row_width) {
                break;
            }
            if (wc == ' ' && col + cw < row_width) {
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
        a->lines[a->nlines - 1].hang = (unsigned char)(cont ? hang : 0);
        a->lines[a->nlines - 1].style = (unsigned char)row_style;
        if (mark) {
            mark_tail(m, base + start, a->nlines - 1, cont, base, code,
                      row_style);
        }
        cont = 1;
        pos = next;
    }
}

static int tool_is_last(App *a, const Msg *m) {
    int p = m->tool_parent;
    if (p < 0 || p >= a->nmsg) {
        return 1;
    }
    return m->tool_pos >= a->msgs[p].ncalls - 1;
}

static void layout_block(App *a, int mi, const char *text, size_t len,
                         int type, int width) {
    if (len == 0) {
        line_push(a, type, mi, 0, 0, 0);
        return;
    }
    size_t pos = 0;
    while (pos < len) {
        size_t nl = pos;
        while (nl < len && text[nl] != '\n') {
            nl++;
        }
        size_t llen = nl - pos;
        if (llen == 0) {
            line_push(a, type, mi, (int)pos, 0, 0);
        } else {
            wrap_run(a, mi, text + pos, llen, pos, type, width, 0, 0, 0, 0,
                     0, 0);
        }
        if (nl == len) {
            break;
        }
        pos = nl + 1;
    }
}

static void layout_tool(App *a, int mi) {
    Msg *m = &a->msgs[mi];
    line_push(a, RL_TOOL_HEAD, mi, 0, 0, 0);
    if (m->tool_open && m->len > 0) {
        line_push(a, RL_TOOL_TOP, mi, 0, 0, 0);
        int w = a->layout_width - 5;
        if (w < 8) {
            w = 8;
        }
        layout_block(a, mi, m->text, m->len, RL_TOOL_BODY, w);
        line_push(a, RL_TOOL_BOT, mi, 0, 0, 0);
    }
    if (tool_is_last(a, m)) {
        line_push(a, RL_GAP, mi, 0, 0, 0);
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
    if (m->role == ROLE_TOOL) {
        layout_tool(a, mi);
        return;
    }
    if (m->role == ROLE_ASSISTANT) {
        md_sync(m, a->unicode);
    }
    const char *body = m->text ? m->text : "";
    size_t blen = m->len;
    int md = 0;
    if (m->role == ROLE_ASSISTANT) {
        body = m->disp ? m->disp : "";
        blen = m->dlen;
        md = 1;
    }
    int inc = incremental && m->lay_valid;
    size_t inc_off = m->lay_off;
    size_t inc_para = m->lay_para;
    int inc_row = m->lay_row;
    int inc_cont = m->lay_cont;
    int inc_code = m->lay_code;
    int inc_text_start = m->lay_text_start;
    unsigned inc_style = m->lay_style;
    m->lay_valid = 0;

    if (m->role == ROLE_NOTE) {
        if (m->len == 0) {
            line_push(a, RL_NOTE, mi, 0, 0, 0);
        } else {
            wrap_run(a, mi, m->text, m->len, 0, RL_NOTE, a->layout_width,
                     0, 0, 0, 0, 0, 0);
        }
        line_push(a, RL_GAP, mi, 0, 0, 0);
        return;
    }
    int first = -1;
    int text_start;
    int in_code = 0;
    size_t pos = 0;
    int reason_stream = a->streaming && mi == a->gen_idx && m->rlen > 0 &&
                        blen == 0 && m->ncalls == 0;
    if (!inc) {
        if (m->role == ROLE_ASSISTANT && m->rlen > 0) {
            line_push(a, RL_REASON_HEAD, mi, 0, 0, 0);
            first = a->nlines - 1;
            if (!m->reason_collapsed) {
                wrap_run(a, mi, m->reason, m->rlen, 0, RL_REASON,
                         a->layout_width, 0, 0, 0, 0, 0, 0);
                for (int i = a->nlines - 1; i >= 0 && a->lines[i].msg == mi;
                     i--) {
                    if (a->lines[i].type == RL_REASON) {
                        a->lines[i].close = 1;
                        break;
                    }
                }
                if (blen > 0) {
                    line_push(a, RL_GAP, mi, 0, 0, 0);
                }
            }
        }
        text_start = a->nlines;
    } else {
        text_start = inc_text_start;
        a->nlines = inc_row;
        if (m->role == ROLE_ASSISTANT && m->rlen > 0 &&
            !m->reason_collapsed && blen > 0 && inc_row == text_start &&
            (text_start == 0 ||
             a->lines[text_start - 1].type != RL_GAP)) {
            line_push(a, RL_GAP, mi, 0, 0, 0);
            text_start = a->nlines;
            m->lay_text_start = text_start;
        }
        pos = inc_para;
        in_code = inc_code;
    }

    if (blen > 0) {
        while (pos < blen) {
            size_t scan = pos;
            if (inc && pos == inc_para && inc_off > scan) {
                scan = inc_off;
            }
            size_t nl = scan;
            while (nl < blen && body[nl] != '\n') {
                nl++;
            }
            size_t llen = nl - pos;
            int line_code = in_code;
            int kind = -1;
            int hang = 0;
            size_t skip = 0;
            int type = in_code ? RL_CODE : RL_TEXT;
            if (md) {
                kind = md_line_meta(body + pos, llen, &hang, &skip);
                line_code = 0;
                in_code = 0;
                type = RL_TEXT;
                if (kind == MDL_CODE) {
                    type = RL_CODE;
                    line_code = 1;
                    in_code = 1;
                } else if (kind == MDL_RULE) {
                    type = RL_RULE;
                } else if (kind == MDL_FENCE_OPEN) {
                    type = RL_CBOX_TOP;
                } else if (kind == MDL_FENCE_CLOSE) {
                    type = RL_CBOX_BOT;
                }
            }
            if (!md && line_is_fence(body + pos, llen)) {
                line_push(a, RL_FENCE, mi, (int)pos, (int)llen, 0);
                mark_tail(m, pos, a->nlines - 1, 0, pos, line_code, 0);
                in_code = !in_code;
            } else if (type == RL_RULE) {
                line_push(a, RL_RULE, mi, (int)(pos + skip), 0, 0);
                mark_tail(m, pos, a->nlines - 1, 0, pos, 0, 0);
            } else if (type == RL_CBOX_TOP || type == RL_CBOX_BOT) {
                line_push(a, type, mi, (int)(pos + skip),
                          (int)(llen - skip), 0);
                if (type == RL_CBOX_BOT) {
                    a->lines[a->nlines - 1].close = 1;
                }
                mark_tail(m, pos, a->nlines - 1, 0, pos, 0, 0);
            } else if (llen == skip) {
                line_push(a, type, mi, (int)(pos + skip), 0, 0);
                mark_tail(m, pos, a->nlines - 1, 0, pos, line_code, 0);
            } else {
                size_t vstart = pos + skip;
                size_t from = 0;
                int cont = 0;
                unsigned style0 = 0;
                if (inc && pos == inc_para && inc_off > vstart) {
                    from = inc_off - vstart;
                    cont = inc_cont;
                    style0 = inc_style;
                }
                wrap_run(a, mi, body + vstart, llen - skip, vstart, type,
                         a->layout_width, from, cont, line_code, 1, hang,
                         style0);
                /* lay_para is a line start, and a markdown line starts at
                 * its metadata prefix.  wrap_run only knows the visible
                 * base, so point the cursor back at pos; otherwise the next
                 * delta fails the newline guard and rebuilds every row. */
                m->lay_para = pos;
            }
            if (nl == blen) {
                break;
            }
            pos = nl + 1;
            mark_tail(m, pos, a->nlines, 0, pos, in_code, 0);
        }
    } else if (!reason_stream &&
               !(m->role == ROLE_ASSISTANT && m->rlen > 0 &&
                 m->reason_collapsed && m->ncalls > 0)) {
        line_push(a, RL_TEXT, mi, 0, 0, 0);
        mark_tail(m, 0, a->nlines - 1, 0, 0, 0, 0);
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
        if (m->role == ROLE_ASSISTANT && m->rlen > 0 && blen > 0 &&
            text_start < a->nlines) {
            a->lines[text_start].head = 1;
        }
        if (m->role == ROLE_ASSISTANT && m->rlen > 0 && blen == 0 &&
            m->ncalls > 0 && !m->reason_collapsed &&
            text_start < a->nlines) {
            a->lines[text_start].head = 1;
        }
        m->lay_text_start = text_start;
    }
    int tool_head_only =
        m->role == ROLE_ASSISTANT && blen == 0 && m->ncalls > 0;
    if (!tool_head_only && !reason_stream) {
        line_push(a, RL_GAP, mi, 0, 0, 0);
    }
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
        if (m->role == ROLE_ASSISTANT) {
            md_sync(m, a->unicode);
        }
        const char *p = m->text ? m->text : "";
        size_t blen = m->len;
        if (m->role == ROLE_ASSISTANT) {
            p = m->disp ? m->disp : "";
            blen = m->dlen;
        }
        if (m->lay_valid && m->lay_off <= blen &&
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

static void mark_relayout(App *a, int mi) {
    if (a->layout_from == 0 || a->layout_from > mi) {
        a->layout_from = mi;
    }
    a->layout_dirty = 1;
    a->dirty = 1;
}

static void reason_toggle(App *a, int mi) {
    Msg *m = &a->msgs[mi];
    if (m->rlen == 0) {
        return;
    }
    m->reason_collapsed = !m->reason_collapsed;
    m->reason_user_set = 1;
    m->lay_valid = 0;
    mark_relayout(a, mi);
}

static void tool_toggle(App *a, int mi) {
    Msg *m = &a->msgs[mi];
    if (m->len == 0) {
        return;
    }
    m->tool_open = !m->tool_open;
    m->lay_valid = 0;
    mark_relayout(a, mi);
}

void chat_toggle_fold(App *a) {
    for (int i = a->nmsg - 1; i >= 0; i--) {
        Msg *m = &a->msgs[i];
        if (m->role == ROLE_TOOL && m->len > 0) {
            tool_toggle(a, i);
            return;
        }
        if (m->role == ROLE_ASSISTANT && m->rlen > 0) {
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
    if (l->msg < 0 || l->msg >= a->nmsg) {
        return 0;
    }
    if (l->type >= RL_TOOL_HEAD && l->type <= RL_TOOL_BOT) {
        tool_toggle(a, l->msg);
        return 1;
    }
    if (l->type != RL_REASON_HEAD && l->type != RL_REASON) {
        return 0;
    }
    reason_toggle(a, l->msg);
    return 1;
}

static void md_attrs(const App *a, int pair, int attr, unsigned mask,
                     int *out_pair, int *out_attr) {
    int p = pair;
    int at = attr;
    if (mask & MD_CODE) {
        if (a->color) {
            p = (int)pair_attr(a, P_CODE);
        } else {
            at |= A_UNDERLINE;
        }
    } else if (mask & MD_LINK) {
        p = (int)pair_attr(a, P_ACCENT);
        at |= A_UNDERLINE;
    } else if (mask & MD_ACCENT) {
        p = (int)pair_attr(a, P_ACCENT);
        at |= A_BOLD;
    }
    if (mask & MD_BOLD) {
        at |= A_BOLD;
    }
    if (mask & MD_ITALIC) {
        at |= A_ITALIC;
    }
    if (mask & (MD_STRIKE | MD_DIM)) {
        at |= A_DIM;
    }
    *out_pair = p;
    *out_attr = at;
}

int draw_slice(App *a, int y, int x, const char *s, int off, int len,
               int maxx, int pair, int attr, unsigned mask, int md,
               int *endx) {
    int cx = x;
    int col = 0;
    int i = off;
    int end = off + len;
    int cur_pair;
    int cur_attr;
    md_attrs(a, pair, attr, mask, &cur_pair, &cur_attr);
    wattrset(stdscr, (attr_t)cur_pair | (attr_t)cur_attr);
    while (i < end && cx <= maxx) {
        unsigned char ch = (unsigned char)s[i];
        if (md && ch == MD_CMD) {
            if (i + 1 < end) {
                unsigned char cmd = (unsigned char)s[i + 1];
                if (cmd & MD_META) {
                    i += 2;
                    if (i < end) {
                        i++;
                    }
                } else {
                    mask = cmd;
                    md_attrs(a, pair, attr, mask, &cur_pair, &cur_attr);
                    wattrset(stdscr,
                             (attr_t)cur_pair | (attr_t)cur_attr);
                    i += 2;
                }
            } else {
                i++;
            }
            continue;
        }
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

static int text_cells(const char *s, int len) {
    int col = 0;
    int i = 0;
    while (i < len) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 32 && ch < 127) {
            col++;
            i++;
            continue;
        }
        wchar_t wc;
        int n = utf8_next(s + i, (size_t)(len - i), &wc);
        if (n <= 0) {
            break;
        }
        if (wc == '\r') {
            i += n;
            continue;
        }
        if (wc == '\t') {
            col += 4 - (col % 4);
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
        col += w;
        i += n;
    }
    return col;
}

static int fit_bytes(const char *s, int len, int cells) {
    int col = 0;
    int i = 0;
    while (i < len) {
        unsigned char ch = (unsigned char)s[i];
        int n;
        int w;
        if (ch >= 32 && ch < 127) {
            n = 1;
            w = 1;
        } else {
            wchar_t wc;
            n = utf8_next(s + i, (size_t)(len - i), &wc);
            if (n <= 0) {
                break;
            }
            if (wc == '\r') {
                i += n;
                continue;
            }
            if (wc == '\t') {
                w = 4 - (col % 4);
            } else if (wc < 32) {
                i += n;
                continue;
            } else {
                w = wcwidth(wc);
                if (w < 0) {
                    w = 1;
                }
            }
        }
        if (col + w > cells) {
            break;
        }
        col += w;
        i += n;
    }
    return i;
}

static int draw_fit(App *a, int y, int x, int maxx, const char *s, int len,
                    int pair, int attr) {
    if (!s || len <= 0) {
        return x;
    }
    int room = maxx - x + 1;
    if (room <= 0) {
        return x;
    }
    int endx = x;
    if (text_cells(s, len) <= room) {
        draw_slice(a, y, x, s, 0, len, maxx, pair, attr, 0, 0, &endx);
        return endx;
    }
    int cut = fit_bytes(s, len, room - 1);
    draw_slice(a, y, x, s, 0, cut, maxx - 1, pair, attr, 0, 0, &endx);
    wattrset(stdscr, (attr_t)pair | (attr_t)attr);
    mvaddstr(y, endx, a->unicode ? "…" : "~");
    return endx + 1;
}

static void fmt_tool_time(double sec, char *out, size_t cap) {
    out[0] = '\0';
    if (sec <= 0) {
        return;
    }
    if (sec < 0.001) {
        snprintf(out, cap, "<1ms");
    } else if (sec < 1) {
        snprintf(out, cap, "%.0fms", sec * 1000.0);
    } else if (sec < 10) {
        snprintf(out, cap, "%.1fs", sec);
    } else {
        snprintf(out, cap, "%.0fs", sec);
    }
}

static void fmt_tool_size(size_t n, char *out, size_t cap) {
    if (n < 1024) {
        snprintf(out, cap, "%zu B", n);
    } else if (n < (1u << 20)) {
        snprintf(out, cap, "%.1f KB", (double)n / 1024.0);
    } else {
        snprintf(out, cap, "%.1f MB", (double)n / (1024.0 * 1024.0));
    }
}

static void tool_meta(const App *a, const Msg *m, char *out, size_t cap,
                      int *fold) {
    char t[32];
    fmt_tool_time(m->tool_sec, t, sizeof t);
    out[0] = '\0';
    *fold = m->len > 0;
    if (m->tool_state == TOOL_VIEW_ASK) {
        snprintf(out, cap, "awaiting y/n");
        *fold = 0;
        return;
    }
    if (m->tool_state == TOOL_VIEW_ERR) {
        if (t[0]) {
            snprintf(out, cap, "error · %s", t);
        } else {
            snprintf(out, cap, "error");
        }
        return;
    }
    int budget = a->layout_width / 3;
    if (budget < 14) {
        budget = 14;
    }
    if (t[0]) {
        snprintf(out, cap, "%s", t);
    }
    if (m->tool_lines > 0) {
        char ln[32];
        snprintf(ln, sizeof ln, "%ld ln", m->tool_lines);
        size_t n = strlen(out);
        if (n + 3 + strlen(ln) + 2 <= (size_t)budget) {
            snprintf(out + n, cap - n, " · %s", ln);
        }
    }
    if (m->tool_bytes > 0) {
        char sz[32];
        fmt_tool_size(m->tool_bytes, sz, sizeof sz);
        size_t n = strlen(out);
        if (n + 3 + strlen(sz) + 2 <= (size_t)budget) {
            snprintf(out + n, cap - n, " · %s", sz);
        }
    }
    if (!out[0]) {
        snprintf(out, cap, "done");
    }
}

static void draw_tool_head(App *a, int y, Msg *m, int maxx) {
    int last = tool_is_last(a, m);
    wattrset(stdscr, pair_attr(a, P_BORDER));
    mvaddstr(y, 2, last ? sym_tree_last(a) : sym_tree_mid(a));
    mvaddstr(y, 3, sym_tree_h(a));

    const char *glyph = sym_tool_ok(a);
    int gp = P_OK;
    if (m->tool_state == TOOL_VIEW_ERR) {
        glyph = sym_tool_err(a);
        gp = P_ERR;
    } else if (m->tool_state == TOOL_VIEW_ASK) {
        glyph = sym_tool_ask(a);
        gp = P_WAIT;
    }
    wattrset(stdscr, pair_attr(a, gp) | A_BOLD);
    mvaddstr(y, 5, glyph);

    char meta[96];
    int fold = 0;
    tool_meta(a, m, meta, sizeof meta, &fold);
    int mw = (int)strlen(meta) + (fold ? 2 : 0);
    int mx = maxx - mw + 1;
    if (mx < 14) {
        mx = 14;
    }
    int title_max = mx - 2;
    if (title_max < 8) {
        title_max = 8;
    }

    const char *label = m->tool_label;
    if (!label) {
        label = m->tool_name ? m->tool_name : "";
    }
    const char *nm = m->tool_name && *m->tool_name ? m->tool_name : "unknown";
    int nlen = (int)strlen(nm);
    int llen = (int)strlen(label);
    if (nlen > llen) {
        nlen = llen;
    }
    int x = draw_fit(a, y, 7, title_max, label, nlen, pair_attr(a, P_ACCENT),
                     A_BOLD);
    if (llen > nlen && x <= title_max) {
        x = draw_fit(a, y, x, title_max, label + nlen, llen - nlen,
                     pair_attr(a, P_DIM), 0);
    }
    if (m->tool_state == TOOL_VIEW_ERR && !m->tool_open && m->len > 0) {
        const char *e = m->text;
        if (strncmp(e, "ERROR: ", 7) == 0) {
            e += 7;
        }
        int n = 0;
        while (e[n] && e[n] != '\n' && n < 191) {
            n++;
        }
        while (n > 0 && e[n - 1] == ' ') {
            n--;
        }
        int ex = x + 2;
        if (n > 0 && ex <= title_max) {
            draw_fit(a, y, ex, title_max, e, n, pair_attr(a, P_ERR), 0);
        }
    }
    if (mx + mw - 1 <= maxx) {
        int mp = P_DIM;
        if (m->tool_state == TOOL_VIEW_ERR) {
            mp = P_ERR;
        } else if (m->tool_state == TOOL_VIEW_ASK) {
            mp = P_WAIT;
        }
        wattrset(stdscr, pair_attr(a, mp));
        mvaddstr(y, mx, meta);
        if (fold) {
            wattrset(stdscr, pair_attr(a, P_ACCENT));
            mvaddstr(y, mx + (int)strlen(meta) + 1,
                     m->tool_open ? sym_fold_open(a) : sym_fold_closed(a));
        }
    }
}

static void draw_hrule(const App *a, int y, int x, int n) {
    if (n <= 0) {
        return;
    }
    wchar_t wc[2] = {a->unicode ? L'─' : L'-', L'\0'};
    cchar_t cc;
    /* Same pair as the corners; whline_set otherwise uses pair 0. */
    short pair = (short)(a->color ? P_BORDER : 0);
    if (setcchar(&cc, wc, WA_NORMAL, pair, NULL) != OK) {
        mvhline(y, x, '-', n);
        return;
    }
    mvhline_set(y, x, &cc, n);
}

static void draw_tool_box(App *a, int y, const RLine *l, Msg *m, int maxx) {
    int last = tool_is_last(a, m);
    wattrset(stdscr, pair_attr(a, P_BORDER));
    if (!last) {
        mvaddstr(y, 2, sym_tree_bar(a));
    }
    if (l->type == RL_TOOL_TOP) {
        mvaddstr(y, 5, sym_box_tl(a));
        draw_hrule(a, y, 6, maxx - 6);
        mvaddstr(y, maxx, sym_box_tr(a));
    } else if (l->type == RL_TOOL_BOT) {
        mvaddstr(y, 5, sym_box_bl(a));
        draw_hrule(a, y, 6, maxx - 6);
        mvaddstr(y, maxx, sym_box_br(a));
    } else {
        mvaddstr(y, 5, sym_box_v(a));
        int pair = m->tool_state == TOOL_VIEW_ERR ? P_ERR : P_BASE;
        draw_slice(a, y, 7, m->text, l->off, l->len, maxx - 1,
                   pair_attr(a, pair), 0, 0, 0, NULL);
        wattrset(stdscr, pair_attr(a, P_BORDER));
        mvaddstr(y, maxx, sym_box_v(a));
    }
}

static int turn_last_assistant(App *a, int mi) {
    for (int i = mi + 1; i < a->nmsg; i++) {
        if (a->msgs[i].role == ROLE_USER) {
            break;
        }
        if (a->msgs[i].role == ROLE_ASSISTANT) {
            return 0;
        }
    }
    return 1;
}

/*
 * Only the last assistant message of a user turn carries the stats, summed
 * over the whole turn: tool rounds and thinking are part of one answer.
 */
static int fmt_turn_stats(App *a, int mi, char *out, size_t cap) {
    out[0] = '\0';
    if (mi < 0 || mi >= a->nmsg) {
        return 0;
    }
    Msg *last = &a->msgs[mi];
    if (last->role != ROLE_ASSISTANT ||
        (!last->has_stats && !last->stopped) ||
        !turn_last_assistant(a, mi)) {
        return 0;
    }
    int start = mi;
    while (start >= 0 && a->msgs[start].role != ROLE_USER) {
        start--;
    }
    long tok = 0;
    double sec = 0;
    int any = 0;
    for (int i = start + 1; i <= mi; i++) {
        Msg *m = &a->msgs[i];
        if (m->role != ROLE_ASSISTANT || !m->has_stats) {
            continue;
        }
        tok += m->ctok;
        sec += m->elapsed;
        any = 1;
    }
    char el[32];
    char stat[64];
    stat[0] = '\0';
    if (any) {
        fmt_elapsed(sec, el, sizeof el);
        if (tok > 0) {
            snprintf(stat, sizeof stat, "%ld tok | %s", tok, el);
        } else {
            snprintf(stat, sizeof stat, "%s", el);
        }
    }
    if (last->stopped && stat[0]) {
        snprintf(out, cap, "stopped | %s", stat);
    } else if (last->stopped) {
        snprintf(out, cap, "stopped");
    } else {
        snprintf(out, cap, "%s", stat);
    }
    return out[0] != '\0';
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
            if (last_gap && gm->role == ROLE_ASSISTANT) {
                char st[96];
                if (fmt_turn_stats(a, l->msg, st, sizeof st)) {
                    int sl = (int)strlen(st);
                    if (sl < cols - 4) {
                        wattrset(stdscr, pair_attr(a, P_DIM));
                        mvaddstr(y, cols - 3 - sl, st);
                    }
                }
            }
            y++;
            continue;
        }
        if (l->type == RL_TOOL_HEAD) {
            draw_tool_head(a, y, &a->msgs[l->msg], maxx);
            y++;
            continue;
        }
        if (l->type >= RL_TOOL_TOP && l->type <= RL_TOOL_BOT) {
            draw_tool_box(a, y, l, &a->msgs[l->msg], maxx);
            y++;
            continue;
        }
        if (l->type == RL_NOTE) {
            wattrset(stdscr, pair_attr(a, P_NOTE));
            if (!l->cont) {
                mvaddstr(y, 2, sym_dot(a));
            }
            draw_slice(a, y, x, a->msgs[l->msg].text, l->off, l->len, maxx,
                       a->color ? pair_attr(a, P_NOTE) : 0, 0, 0, 0, &endx);
            y++;
            continue;
        }
        Msg *m = &a->msgs[l->msg];
        if (l->type == RL_RULE) {
            wattrset(stdscr, pair_attr(a, P_BORDER) | A_DIM);
            draw_hrule(a, y, 4, maxx - 3);
            y++;
            continue;
        }
        if (l->type == RL_CBOX_TOP || l->type == RL_CBOX_BOT) {
            const char *box = m->role == ROLE_ASSISTANT
                                  ? (m->disp ? m->disp : "")
                                  : m->text;
            wattrset(stdscr, pair_attr(a, P_BORDER));
            mvaddstr(y, 2, l->type == RL_CBOX_TOP ? sym_box_tl(a)
                                                  : sym_box_bl(a));
            draw_hrule(a, y, 3, maxx - 2);
            if (l->type == RL_CBOX_TOP && l->len > 0) {
                wattrset(stdscr, pair_attr(a, P_FENCE));
                mvaddstr(y, 4, " ");
                draw_slice(a, y, 5, box, l->off, l->len, maxx,
                           a->color ? pair_attr(a, P_FENCE) : 0, 0, 0, 0,
                           NULL);
            }
            y++;
            continue;
        }
        int gut_pair = m->role == ROLE_USER ? P_USER : P_AI;
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
        if (l->head && m->role == ROLE_ASSISTANT && l->type == RL_TEXT &&
            m->len == 0 && m->ncalls > 0) {
            char nb[48];
            snprintf(nb, sizeof nb, "  %d tool call%s", m->ncalls,
                     m->ncalls == 1 ? "" : "s");
            wattrset(stdscr, pair_attr(a, P_DIM));
            mvaddstr(y, 4, nb);
        }
        const char *gut = sym_gutter(a);
        const char *text = m->text;
        if (m->role == ROLE_ASSISTANT) {
            text = m->disp ? m->disp : "";
        }
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
            gut = l->close ? sym_reason_close(a) : sym_reason_gutter(a);
            text = m->reason;
            text_pair = P_DIM;
        } else if (l->type == RL_CODE) {
            text_pair = P_CODE;
        } else if (l->type == RL_FENCE) {
            text_pair = P_FENCE;
        }
        if (l->type == RL_CODE) {
            wattrset(stdscr, pair_attr(a, P_FENCE));
            mvaddstr(y, 2, sym_box_v(a));
        }
        int show_gutter =
            !l->head && l->type != RL_CODE &&
            !(m->role == ROLE_ASSISTANT && l->type != RL_REASON);
        if (show_gutter) {
            wattrset(stdscr, pair_attr(a, gut_pair) | A_DIM);
            mvaddstr(y, 2, gut);
        }
        int tx = x + l->hang;
        if (l->type == RL_REASON && l->close) {
            tx++;
        }
        draw_slice(a, y, tx, text, off, len, maxx,
                   a->color ? pair_attr(a, text_pair) : 0, text_attr,
                   l->style, m->role == ROLE_ASSISTANT, &endx);
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

