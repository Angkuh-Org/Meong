#ifndef MEONG_TOOLS_H
#define MEONG_TOOLS_H

#include <stddef.h>

#include "tools/perm.h"
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
 * Decide one operation request. The callback fills req->resolved and returns
 * PERM_ALLOW, PERM_DENY or PERM_ASK. The tool layer copies an ASK into the
 * caller's ToolAsk; the callback never owns the record.
 */
typedef enum PermDecision (*ToolAuthorizeFn)(void *ud, PermReq *req);

/* The static, byte-stable OpenAI tools array. */
const char *tools_json(void);

/*
 * Execute one tool call. The class comes from the registration table, not
 * from the caller. On TOOL_NEED_PERM, ask receives the operation record and
 * nothing is executed. On TOOL_ERR, err receives a model-facing message.
 */
int tool_execute(const char *name, const char *args, size_t args_len,
                 void *ud, ToolAuthorizeFn authorize, ToolAsk *ask,
                 Buf *out, char *err, size_t err_cap);

/*
 * Bound a result for the transcript: inline head/tail plus a notice. When
 * the text exceeds TOOL_INLINE_MAX, the full text is written under spill_dir.
 */
int tool_bound_result(const char *call_id, const char *text, size_t len,
                      const char *spill_dir, Buf *out,
                      char *err, size_t err_cap);

void tool_call_free(ToolCall *call);

/*
 * Compact, human-facing argument summary for one call, e.g.
 * "src/ui/view.c:170-190" or "\"ROLE_TOOL\"  src/".  The caller prepends the
 * tool name.  This is display-only; the wire keeps the raw JSON arguments.
 */
void tool_call_summary(const char *name, const char *args, size_t args_len,
                       const char *cwd, char *out, size_t cap);

#endif
