#ifndef MEONG_APP_H
#define MEONG_APP_H

#include <stddef.h>
#include <stdio.h>
#include <wchar.h>

#include "net/net.h"
#include "util/util.h"

#define APP_NAME "Meong"
#ifndef APP_VERSION
#define APP_VERSION "0.1.1"
#endif
#define DEFAULT_MODEL "cc/deepseek/deepseek-v4.1-flash"

#define MEONG_INPUT_MAX 2048
#define MAX_HISTORY 100
#define MAX_TEXT (4u << 20)
#define CHAT_BUDGET (200u << 10)

enum {
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_NOTE,
    ROLE_ERROR,
};

typedef struct {
    int role;
    char *text;
    size_t len;
    size_t cap;
    size_t chars;
    char *reason;
    size_t rlen;
    size_t rcap;
    int streaming;
    int stopped;
    int has_stats;
    long ptok;
    long ctok;
    double elapsed;
} Msg;

/* ui/view.c owns the layout row definition. */
typedef struct RLine RLine;

typedef struct {
    Endpoint ep;
    char *url;
    char *model;
    char *system_prompt;
    Msg *msgs;
    int nmsg;
    int msgcap;
    char **models;
    int nmodels;

    int picker_open;
    int picker_sel;
    int picker_scroll;
    char picker_filter[96];
    int picker_filter_len;
    int *pick_matches;
    int pick_n;
    int help_open;
    int initial_picker;

    Stream *stream;
    int streaming;
    int gen_idx;
    double gen_start;
    double last_anim;
    long gen_ptok;
    long gen_ctok;
    char stream_err[1024];
    int stream_failed;

    char **queued;
    int nqueued;
    int queued_cap;

    int follow;
    int top;
    int new_below;
    RLine *lines;
    int nlines;
    int linecap;
    int layout_width;
    int layout_from;
    int layout_dirty;
    double last_layout;

    wchar_t input[MEONG_INPUT_MAX];
    size_t ilen;
    size_t icur;
    wchar_t *hist[MAX_HISTORY];
    int nhist;
    int hist_pos;
    wchar_t *draft;

    int quit;
    int dirty;
    int color;
    int unicode;
    int ascii;
    int mouse;
    int spinner;
    int cursor_on;
    int cur_y;
    int cur_x;
    int have_gen_stats;
    int last_trimmed;
    long last_ptok;
    long last_ctok;
    double last_elapsed;
    char models_warn[1024];
} App;

void *xcalloc(size_t n, size_t sz);

char *join_path(const char *dir, const char *name);
char *config_dir(void);
char *cache_dir(void);
char *state_file(void);
char *state_load_model(void);
void state_save_model(const char *model);

void msgs_clear(App *a);
int msg_add(App *a, int role, const char *text);
void msg_set(Msg *m, const char *text);
int msg_append(Msg *m, const char *s, size_t n);
int msg_append_reason(Msg *m, const char *s, size_t n);
void trim_trailing_ws(Msg *m);

void queue_push(App *a, const char *text);
char *queue_pop(App *a);
void queue_clear(App *a);

#endif
