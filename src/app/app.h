#ifndef MEONG_APP_H
#define MEONG_APP_H

#include <stddef.h>
#include <stdio.h>
#include <wchar.h>

#include "json/json.h"
#include "chat/tool_accum.h"
#include "net/net.h"
#include "tools/tools.h"
#include "util/util.h"

#define APP_NAME "Meong"
#ifndef APP_VERSION
#define APP_VERSION "0.11.0"
#endif
#define DEFAULT_MODEL "cc/deepseek/deepseek-v4.1-flash"

#define MEONG_INPUT_MAX 2048
#define MAX_HISTORY 100
#define MAX_TEXT (4u << 20)
#define CHAT_BUDGET (200u << 10)

/* Cached for the footer; the sentinel means "recompute on next draw". */
#define MODEL_CTX_UNSET (-2)

enum {
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_NOTE,
    ROLE_TOOL,
};

/* Display state of a ROLE_TOOL row; the wire only ever sees its text. */
enum {
    TOOL_VIEW_OK,
    TOOL_VIEW_ERR,
    TOOL_VIEW_ASK,
};

/* The retry button replays whichever request produced the error. */
enum {
    ERR_RETRY_CHAT,
    ERR_RETRY_MODELS,
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
    size_t lay_off;
    size_t lay_para;
    int lay_row;
    int lay_cont;
    int lay_code;
    int lay_valid;
    int lay_text_start;
    int lay_cursor;
    int has_stats;
    int reason_collapsed;
    int reason_user_set;
    long ptok;
    long ctok;
    long cached_tok;
    unsigned char has_cached;
    double elapsed;
    ToolCall *calls;
    int ncalls;
    int calls_cap;
    size_t args_bytes;
    char *call_id;
    char *tool_name;
    char *tool_label; /* name + flat argument summary; never sent */
    int tool_parent;  /* assistant message that owns this call */
    int tool_pos;     /* position in that message's calls[] */
    int tool_state;
    int tool_open;
    double tool_sec;
    long tool_lines;
    size_t tool_bytes;
} Msg;

/* ui/view.c owns the layout row definition. */
typedef struct RLine RLine;

/*
 * Capability fields are tri-state: -1 means the gateway did not report the
 * field, 0 and 1 are explicit upstream answers.
 */
typedef struct {
    long long context_window;
    long long max_output_tokens;
    double in_cost;
    double out_cost;
    signed char vision;
    signed char function_calling;
    signed char reasoning;
    signed char prompt_caching;
    signed char estimated;
} ModelCaps;

typedef struct {
    Endpoint ep;
    char *url;
    char *model;
    char *system_prompt;
    char *cwd;
    Msg *msgs;
    int nmsg;
    int msgcap;
    char **models;
    ModelCaps *model_caps;
    int nmodels;

    int picker_open;
    int picker_sel;
    int picker_scroll;
    char picker_filter[96];
    int picker_filter_len;
    int *pick_matches;
    int pick_n;
    int help_open;
    int pet_open;
    int pet_count;
    int initial_picker;
    int cmd_sel;
    int cmd_hide;
    int cmd_y0;
    int cmd_x0;
    int cmd_w;
    int cmd_h;

    Stream *stream;
    JDoc stream_doc;
    Buf stream_buf;
    int streaming;
    int gen_idx;
    double gen_start;
    double last_anim;
    long gen_ptok;
    long gen_ctok;
    long gen_cached;
    unsigned char gen_has_cached;
    /* TODO(dead): written by stream_apply/stream_apply_json, never read;
     * finish_generation keys on m->ncalls. Remove it or wire a consumer. */
    int gen_finish_tool;
    ToolAccum tool_accum;
    int tool_round;
    int tool_msg_idx;
    int tool_call_pos;
    int tool_pending;
    int tool_allow_all;
    char perm_path[1024];
    int tools_enabled;
    int yolo;
    char stream_err[1024];
    int stream_failed;

    char **queued;
    int nqueued;
    int queued_cap;

    int follow;
    int top;
    int new_below;
    int chat_y0;
    int chat_top;
    int chat_end;
    int brand_y;
    int brand_x0;
    int brand_x1;
    int model_y;
    int model_x0;
    int model_x1;
    int model_open;
    int model_py0;
    int model_py1;
    int model_px0;
    int model_px1;
    int error_open;
    int error_sel;
    int error_action;
    char error_text[2048];
    int error_y0;
    int error_y1;
    int error_x0;
    int error_x1;
    int error_retry_y;
    int error_close_y;
    int last_failed;
    int pet_y0;
    int pet_y1;
    int pet_x0;
    int pet_x1;
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
    int nerd;
    int ascii;
    int mouse;
    int cursor_on;
    int cur_y;
    int cur_x;
    int have_gen_stats;
    long long model_ctx;
    int last_trimmed;
    long last_ptok;
    long last_ctok;
    long last_cached;
    unsigned char last_has_cached;
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
void msg_release(Msg *m);
int msg_add(App *a, int role, const char *text);
void msg_set(Msg *m, const char *text);
int msg_append(Msg *m, const char *s, size_t n);
int msg_append_reason(Msg *m, const char *s, size_t n);
void trim_trailing_ws(Msg *m);

void queue_push(App *a, const char *text);
char *queue_pop(App *a);
void queue_clear(App *a);

#endif
