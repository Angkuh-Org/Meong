#include "chat/title.h"

#include <string.h>

#include "api/api.h"
#include "json/json.h"
#include "net/net.h"

#define TITLE_PROMPT_MAX 4000
#define TITLE_IDLE_MS 60000

static const char TITLE_SYSTEM[] =
    "Write a title for this chat from the user's first message. Reply with "
    "only the title: at most 5 words, no quotes, no trailing punctuation, in "
    "the language of the message.";

/* One title line: drop wrapping quotes and a trailing period, cut on a
 * UTF-8 boundary, and never exceed the header buffer. */
static void title_store(App *a, const char *s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ' ||
                     s[i] == '\t')) {
        i++;
    }
    size_t end = i;
    while (end < n && s[end] != '\n' && s[end] != '\r') {
        end++;
    }
    while (end > i && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        end--;
    }
    if (end - i >= 2) {
        if ((s[i] == '"' && s[end - 1] == '"') ||
            (s[i] == '\'' && s[end - 1] == '\'') ||
            (s[i] == '`' && s[end - 1] == '`')) {
            i++;
            end--;
        } else if (end - i >= 6 && !memcmp(s + i, "\xE2\x80\x9C", 3) &&
                   !memcmp(s + end - 3, "\xE2\x80\x9D", 3)) {
            i += 3;
            end -= 3;
        }
    }
    while (end > i && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        end--;
    }
    if (end > i && s[end - 1] == '.') {
        end--;
        while (end > i && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
            end--;
        }
    }
    if (end <= i) {
        return;
    }
    size_t cap = sizeof a->title - 1;
    size_t cut = end - i;
    if (cut > cap) {
        cut = cap;
        while (cut > 0 && ((unsigned char)s[i + cut] & 0xC0) == 0x80) {
            cut--;
        }
    }
    if (cut == 0) {
        return;
    }
    memcpy(a->title, s + i, cut);
    a->title[cut] = '\0';
    a->dirty = 1;
}

/* PAIG's opencode free route forces stream:true even when the body says
 * false, so the reply arrives as SSE deltas; a non-free route answers with a
 * plain JSON body. Collect both and let title_tick choose the source. */
static int title_on_event(void *ud, const char *data, size_t len) {
    App *a = ud;
    JDelta fd;
    if (jdelta_scan(data, len, &fd) == 0) {
        if (fd.has_content) {
            buf_clear(&a->stream_buf);
            json_unescape(fd.content, fd.content_len, &a->stream_buf);
            if (a->stream_buf.len > 0) {
                buf_add(&a->title_buf, a->stream_buf.p, a->stream_buf.len);
            }
        }
        return 0;
    }
    JDoc d = {0};
    if (jdoc_parse(&d, data, len) == 0) {
        int c0 = jgeti(&d, jget(&d, jdoc_root(&d), "choices"), 0);
        int content = jget(&d, jget(&d, c0, "message"), "content");
        if (content >= 0 && jtype(&d, content) == J_STRING) {
            buf_clear(&a->stream_buf);
            if (jstring(&d, content, &a->stream_buf) == 0 &&
                a->stream_buf.len > 0) {
                buf_add(&a->title_buf, a->stream_buf.p, a->stream_buf.len);
            }
        }
        jdoc_free(&d);
    }
    return 0;
}

void title_request(App *a, const char *prompt) {
    if (!a || a->title_tried || !prompt || !*prompt || !a->model ||
        !a->model[0]) {
        return;
    }
    a->title_tried = 1;
    buf_clear(&a->title_buf);
    size_t pn = strlen(prompt);
    if (pn > TITLE_PROMPT_MAX) {
        pn = TITLE_PROMPT_MAX;
        while (pn > 0 && ((unsigned char)prompt[pn] & 0xC0) == 0x80) {
            pn--;
        }
    }
    Buf body;
    buf_init(&body);
    buf_adds(&body, "{\"model\":");
    buf_json_str(&body, a->model, strlen(a->model));
    buf_adds(&body,
             ",\"stream\":false,\"messages\":["
             "{\"role\":\"system\",\"content\":");
    buf_json_str(&body, TITLE_SYSTEM, strlen(TITLE_SYSTEM));
    buf_adds(&body, "},{\"role\":\"user\",\"content\":");
    buf_json_str(&body, prompt, pn);
    buf_adds(&body, "}]}");
    Stream *s = NULL;
    /* A failed title is silent, so skip the error buffer. */
    int r = stream_start_async(&s, &a->ep, "POST", "/v1/chat/completions",
                               body.p, body.len, 1, title_on_event, a,
                               TITLE_IDLE_MS, NULL, 0);
    buf_free(&body);
    if (r != 0) {
        return;
    }
    a->title_stream = s;
}

void title_tick_slow(App *a) {
    if (!a || !a->title_stream) {
        return;
    }
    int r = stream_tick(a->title_stream, NULL);
    if (r == 0) {
        return;
    }
    if (r > 0 && stream_status(a->title_stream) == 200) {
        if (stream_was_sse(a->title_stream)) {
            if (a->title_buf.len > 0) {
                title_store(a, a->title_buf.p, a->title_buf.len);
            }
        } else {
            const Buf *b = stream_body(a->title_stream);
            if (b && b->p && b->len) {
                Buf t;
                buf_init(&t);
                if (extract_chat_text(b->p, b->len, &t, NULL, NULL, NULL) &&
                    t.len > 0) {
                    title_store(a, t.p, t.len);
                }
                buf_free(&t);
            }
        }
    }
    buf_clear(&a->title_buf);
    stream_free(a->title_stream);
    a->title_stream = NULL;
    a->dirty = 1;
}

void title_reset(App *a) {
    if (!a) {
        return;
    }
    if (a->title_stream) {
        stream_free(a->title_stream);
        a->title_stream = NULL;
    }
    a->title_tried = 0;
    a->title[0] = '\0';
    buf_free(&a->title_buf);
    a->dirty = 1;
}
