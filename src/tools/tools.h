#ifndef MEONG_TOOLS_H
#define MEONG_TOOLS_H

#include <stddef.h>

#include "util/util.h"

#define TOOL_MAX_CALLS 8
#define TOOL_MAX_ARGS (64u << 10)
#define TOOL_INLINE_MAX (16u << 10)

typedef struct {
    char *id;
    char *name;
    char *args;
    size_t args_len;
    size_t args_cap;
} ToolCall;

enum {
    TOOL_ERR = -1,
    TOOL_OK = 0,
    TOOL_NEED_PERM = 1,
};

/*
 * Authorize one filesystem path. 1 allows, 0 denies, -1 asks the user.
 * The caller may invoke the tool again with a callback that answers.
 */
typedef int (*ToolAuthorizeFn)(void *ud, const char *path);

/* The static, byte-stable OpenAI tools array. */
const char *tools_json(void);

/*
 * Execute one tool call. On TOOL_NEED_PERM, ask_path receives the path that
 * needs approval and nothing is executed. On TOOL_ERR, err receives a
 * model-facing message. out is the unbounded result text.
 */
int tool_execute(const char *name, const char *args, size_t args_len,
                 void *ud, ToolAuthorizeFn authorize,
                 char *ask_path, size_t ask_cap,
                 Buf *out, char *err, size_t err_cap);

/*
 * Bound a result for the transcript: inline head/tail plus a notice. When
 * the text exceeds TOOL_INLINE_MAX, the full text is written under spill_dir.
 */
int tool_bound_result(const char *call_id, const char *text, size_t len,
                      const char *spill_dir, Buf *out,
                      char *err, size_t err_cap);

void tool_call_free(ToolCall *call);

#endif
