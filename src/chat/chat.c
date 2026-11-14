#include "chat/chat.h"

#include <stdlib.h>
#include <string.h>

#include "api/api.h"
#include "json/json.h"
#include "ui/ui.h"

static int build_chat_body(App *a, Buf *b, int *trimmed_out) {
    int start = a->nmsg;
    size_t used = 0;
    int first_msg = 1;
    for (int i = a->nmsg - 1; i >= 0; i--) {
        Msg *m = &a->msgs[i];
        if (m->role != ROLE_USER && m->role != ROLE_ASSISTANT) {
            continue;
        }
        if (m->role == ROLE_ASSISTANT && m->len == 0) {
            continue;
        }
        if (!first_msg && used + m->len > CHAT_BUDGET) {
            break;
        }
        used += m->len;
        start = i;
        first_msg = 0;
    }
    while (start < a->nmsg && a->msgs[start].role != ROLE_USER) {
        start++;
    }
    int trimmed = 0;
    for (int i = 0; i < start; i++) {
        if ((a->msgs[i].role == ROLE_USER ||
             a->msgs[i].role == ROLE_ASSISTANT) &&
            a->msgs[i].len > 0) {
            trimmed = 1;
            break;
        }
    }

    buf_adds(b, "{\"model\":");
    buf_json_str(b, a->model, strlen(a->model));
    buf_adds(b, ",\"stream\":true,\"messages\":[");
    int first = 1;
    if (trimmed || (a->system_prompt && *a->system_prompt)) {
        Buf sys;
        buf_init(&sys);
        if (trimmed) {
            buf_adds(&sys,
                     "Earlier turns were omitted to fit the context window.");
        }
        if (a->system_prompt && *a->system_prompt) {
            if (sys.len > 0) {
                buf_adds(&sys, "\n\n");
            }
            buf_adds(&sys, a->system_prompt);
        }
        buf_adds(b, "{\"role\":\"system\",\"content\":");
        buf_json_str(b, sys.p ? sys.p : "", sys.len);
        buf_adds(b, "}");
        buf_free(&sys);
        first = 0;
    }
    for (int i = start; i < a->nmsg; i++) {
        Msg *m = &a->msgs[i];
        if (m->role != ROLE_USER && m->role != ROLE_ASSISTANT) {
            continue;
        }
        if (m->role == ROLE_ASSISTANT && m->len == 0) {
            continue;
        }
        if (!first) {
            buf_addc(b, ',');
        }
        first = 0;
        buf_adds(b, "{\"role\":");
        buf_adds(b, m->role == ROLE_USER ? "\"user\"" : "\"assistant\"");
        buf_adds(b, ",\"content\":");
        buf_json_str(b, m->text ? m->text : "", m->len);
        buf_addc(b, '}');
    }
    buf_adds(b, "]}");
    if (trimmed_out) {
        *trimmed_out = trimmed;
    }
    return 0;
}

static void stream_apply(App *a, const JDelta *fd) {
    if (fd->has_ptok && fd->prompt_tokens >= 0) {
        a->gen_ptok = (long)fd->prompt_tokens;
    }
    if (fd->has_ctok && fd->completion_tokens >= 0) {
        a->gen_ctok = (long)fd->completion_tokens;
    }
    if (a->gen_idx < 0 || a->gen_idx >= a->nmsg) {
        return;
    }
    Msg *m = &a->msgs[a->gen_idx];
    if (fd->has_content) {
        buf_clear(&a->stream_buf);
        json_unescape(fd->content, fd->content_len, &a->stream_buf);
        if (a->stream_buf.len > 0 &&
            msg_append(m, a->stream_buf.p, a->stream_buf.len) != 0) {
            snprintf(a->stream_err, sizeof a->stream_err,
                     "the reply grew past %u bytes", MAX_TEXT);
            a->stream_failed = 1;
        }
    }
    if (fd->has_reason) {
        buf_clear(&a->stream_buf);
        json_unescape(fd->reason, fd->reason_len, &a->stream_buf);
        if (a->stream_buf.len > 0) {
            msg_append_reason(m, a->stream_buf.p, a->stream_buf.len);
        }
    }
}

static int stream_apply_json(App *a, const char *data, size_t len) {
    JDoc *d = &a->stream_doc;
    if (jdoc_parse(d, data, len) != 0) {
        return -1;
    }
    int root = jdoc_root(d);
    int er = jget(d, root, "error");
    if (er >= 0 && !a->stream_failed) {
        extract_api_error(data, len, a->stream_err, sizeof a->stream_err);
        a->stream_failed = 1;
    }
    int usage = jget(d, root, "usage");
    if (usage >= 0) {
        long long v = jint(d, jget(d, usage, "prompt_tokens"), -1);
        if (v >= 0) {
            a->gen_ptok = (long)v;
        }
        v = jint(d, jget(d, usage, "completion_tokens"), -1);
        if (v >= 0) {
            a->gen_ctok = (long)v;
        }
    }
    int choices = jget(d, root, "choices");
    int c0 = jgeti(d, choices, 0);
    if (c0 >= 0) {
        int delta = jget(d, c0, "delta");
        if (delta >= 0 && a->gen_idx >= 0 && a->gen_idx < a->nmsg) {
            Msg *m = &a->msgs[a->gen_idx];
            int content = jget(d, delta, "content");
            if (content >= 0 && jtype(d, content) == J_STRING) {
                buf_clear(&a->stream_buf);
                if (jstring(d, content, &a->stream_buf) == 0 &&
                    a->stream_buf.len > 0) {
                    if (msg_append(m, a->stream_buf.p,
                                   a->stream_buf.len) != 0) {
                        snprintf(a->stream_err, sizeof a->stream_err,
                                 "the reply grew past %u bytes", MAX_TEXT);
                        a->stream_failed = 1;
                    }
                }
            }
            int reason = jget(d, delta, "reasoning_content");
            if (reason < 0) {
                reason = jget(d, delta, "reasoning");
            }
            if (reason >= 0 && jtype(d, reason) == J_STRING) {
                buf_clear(&a->stream_buf);
                if (jstring(d, reason, &a->stream_buf) == 0 &&
                    a->stream_buf.len > 0) {
                    msg_append_reason(m, a->stream_buf.p, a->stream_buf.len);
                }
            }
        }
    }
    return 0;
}

static int on_stream_event(void *ud, const char *data, size_t len) {
    App *a = ud;
    JDelta fd;
    if (jdelta_scan(data, len, &fd) == 0) {
        stream_apply(a, &fd);
    } else if (stream_apply_json(a, data, len) != 0) {
        return 0;
    }
    a->layout_dirty = 1;
    if (a->gen_idx >= 0 && a->layout_from > a->gen_idx) {
        a->layout_from = a->gen_idx;
    }
    a->dirty = 1;
    if (!a->follow) {
        a->new_below = 1;
    }
    return 0;
}

int start_generation(App *a) {
    Buf body;
    buf_init(&body);
    int trimmed = 0;
    build_chat_body(a, &body, &trimmed);
    a->last_trimmed = trimmed;
    char err[512];
    a->gen_ptok = 0;
    a->gen_ctok = 0;
    a->stream_err[0] = '\0';
    a->stream_failed = 0;
    int idx = msg_add(a, ROLE_ASSISTANT, "");
    a->gen_idx = idx;
    /* Rebuild from the last user turn; /retry's stale rows sit above it. */
    a->layout_from = idx > 0 ? idx - 1 : 0;
    Stream *s = NULL;
    if (stream_start(&s, &a->ep, "POST", "/v1/chat/completions", body.p,
                     body.len, 1, on_stream_event, a, 240000, err,
                     sizeof err) != 0) {
        Buf msg;
        buf_init(&msg);
        buf_printf(&msg, "cannot reach %s\n%s", a->url, err);
        if (strstr(err, "refused")) {
            buf_adds(&msg, "\n\nis the gateway running?  try: paig --start");
        }
        a->nmsg = idx;
        a->gen_idx = -1;
        a->streaming = 0;
        a->stream = NULL;
        a->layout_dirty = 1;
        a->layout_from = 0;
        a->dirty = 1;
        buf_free(&body);
        error_open(a, msg.p ? msg.p : err, ERR_RETRY_CHAT);
        buf_free(&msg);
        return -1;
    }
    buf_free(&body);
    a->stream = s;
    a->streaming = 1;
    a->gen_start = now_sec();
    a->last_anim = a->gen_start;
    a->follow = 1;
    a->new_below = 0;
    a->dirty = 1;
    return 0;
}

int finish_generation(App *a, int ok) {
    if (!a->stream) {
        a->streaming = 0;
        return 0;
    }
    Stream *s = a->stream;
    if (a->gen_idx < 0 || a->gen_idx >= a->nmsg) {
        stream_free(s);
        a->stream = NULL;
        a->streaming = 0;
        a->layout_dirty = 1;
        a->layout_from = 0;
        a->dirty = 1;
        return 0;
    }
    int status = stream_status(s);
    const Buf *body = stream_body(s);
    char emsg[1400];
    emsg[0] = '\0';
    if (ok && status != 200) {
        ok = 0;
        format_http_error(status, body, emsg, sizeof emsg);
    }
    if (ok && a->stream_failed) {
        ok = 0;
        snprintf(emsg, sizeof emsg, "%s",
                 a->stream_err[0] ? a->stream_err : "stream failed");
    }
    if (!ok && !emsg[0]) {
        if (a->stream_err[0]) {
            snprintf(emsg, sizeof emsg, "%s", a->stream_err);
        } else if (stream_error(s)[0]) {
            snprintf(emsg, sizeof emsg, "%s", stream_error(s));
        } else if (status && status != 200) {
            format_http_error(status, body, emsg, sizeof emsg);
        } else {
            snprintf(emsg, sizeof emsg, "stream failed");
        }
    }
    Msg *m = &a->msgs[a->gen_idx];
    if (ok && status == 200 && !stream_was_sse(s) && body && body->p &&
        body->len) {
        char raw_err[1024];
        if (extract_api_error(body->p, body->len, raw_err,
                              sizeof raw_err)) {
            ok = 0;
            snprintf(emsg, sizeof emsg, "%s", raw_err);
        } else {
            Buf txt;
            buf_init(&txt);
            if (extract_chat_text(body->p, body->len, &txt, &a->gen_ptok,
                                  &a->gen_ctok)) {
                if (m->len > 0) {
                    msg_append(m, "\n", 1);
                }
                if (msg_append(m, txt.p, txt.len) != 0) {
                    ok = 0;
                    snprintf(emsg, sizeof emsg, "the reply is too large");
                }
            }
            buf_free(&txt);
        }
    }
    m->streaming = 0;
    if (ok) {
        trim_trailing_ws(m);
        if (m->len == 0 && m->rlen == 0) {
            ok = 0;
            snprintf(emsg, sizeof emsg, "the model returned an empty reply");
        } else {
            if (m->rlen > 0 && m->len > 0 && !m->reason_user_set) {
                m->reason_collapsed = 1;
            }
            m->elapsed = now_sec() - a->gen_start;
            m->ptok = a->gen_ptok;
            m->ctok = a->gen_ctok;
            m->has_stats = 1;
            a->have_gen_stats = 1;
            a->last_ptok = a->gen_ptok;
            a->last_ctok = a->gen_ctok;
            a->last_elapsed = m->elapsed;
        }
    }
    if (!ok && m->len == 0 && m->rlen == 0) {
        /* TODO(fix): the guard cannot be false today (msg_add is blocked
         * while streaming). If it ever can, the frees below leave a live
         * Msg with dangling text/reason; reset both and their lengths
         * before dropping the slot. */
        free(m->text);
        free(m->reason);
        if (a->gen_idx == a->nmsg - 1) {
            a->nmsg = a->gen_idx;
        }
        a->gen_idx = -1;
    }
    stream_free(s);
    a->stream = NULL;
    a->streaming = 0;
    a->layout_dirty = 1;
    a->layout_from = 0;
    a->dirty = 1;
    if (!a->follow) {
        a->new_below = 1;
    }
    if (!ok) {
        error_open(a, emsg[0] ? emsg : "request failed", ERR_RETRY_CHAT);
    } else {
        a->last_failed = 0;
    }
    return ok;
}

void cancel_generation(App *a) {
    if (!a->streaming || !a->stream) {
        return;
    }
    if (a->gen_idx >= 0 && a->gen_idx < a->nmsg) {
        Msg *m = &a->msgs[a->gen_idx];
        m->streaming = 0;
        if (m->len == 0 && m->rlen == 0) {
            m->role = ROLE_NOTE;
            free(m->reason);
            m->reason = NULL;
            m->rlen = 0;
            m->rcap = 0;
            msg_set(m, "generation stopped");
        } else {
            m->stopped = 1;
        }
    }
    stream_free(a->stream);
    a->stream = NULL;
    a->streaming = 0;
    a->layout_dirty = 1;
    a->layout_from = 0;
    a->dirty = 1;
    if (!a->follow) {
        a->new_below = 1;
    }
}

static int send_text(App *a, const char *text) {
    if (text[0] == '/') {
        run_command(a, text + 1);
        return a->streaming ? 1 : 0;
    }
    msg_add(a, ROLE_USER, text);
    if (start_generation(a) != 0) {
        return -1;
    }
    a->follow = 1;
    a->new_below = 0;
    return 1;
}

void drain_queue(App *a) {
    while (!a->streaming && a->nqueued > 0) {
        char *text = queue_pop(a);
        int r = send_text(a, text);
        free(text);
        if (r != 0 || a->quit) {
            break;
        }
    }
    a->dirty = 1;
}

void submit_input(App *a) {
    if (a->ilen == 0) {
        if (!a->streaming && a->nqueued > 0) {
            drain_queue(a);
        }
        return;
    }
    char *raw = wcs_to_utf8(a->input, a->ilen);
    char *text = trim_ws(raw);
    if (!*text) {
        free(raw);
        return;
    }
    char *owned = xstrdup(text);
    wchar_t input_copy[MEONG_INPUT_MAX];
    size_t input_n = a->ilen;
    memcpy(input_copy, a->input, input_n * sizeof input_copy[0]);
    history_add(a, input_copy, input_n);
    input_reset(a);
    if (a->streaming || a->nqueued > 0) {
        queue_push(a, owned);
    } else {
        send_text(a, owned);
    }
    free(owned);
    free(raw);
    if (!a->streaming && a->nqueued > 0) {
        drain_queue(a);
    }
}

