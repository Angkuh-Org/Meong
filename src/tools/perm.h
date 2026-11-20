#ifndef MEONG_TOOLS_PERM_H
#define MEONG_TOOLS_PERM_H

#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TOOL_ASK_TEXT_MAX 512

/*
 * One action class per tool. The class is bound where the tool is registered,
 * never inferred from the model's tool-name string or from a handler's prose.
 */
enum ToolAction {
    TOOL_ACT_READ,
    TOOL_ACT_MUTATE,
    TOOL_ACT_EXEC,
};

/* The numeric values match the old 1/0/-1 callback contract. */
enum PermDecision {
    PERM_DENY = 0,
    PERM_ALLOW = 1,
    PERM_ASK = -1,
};

enum ToolAskKind {
    TOOL_ASK_PATH,
    TOOL_ASK_COMMAND,
    TOOL_ASK_MUTATE,
};

/*
 * Input to one permission decision. The owner fills action/tool/path/command/
 * preview; perm_decide fills resolved whenever the decision needs the
 * canonical path (every ASK, plus the inside/outside read check). Pointers are
 * valid only for the call.
 */
typedef struct {
    enum ToolAction action;
    const char *tool;
    const char *path;
    const char *command;
    const char *preview;
    size_t preview_len;
    /*
     * TODO(mem): every caller reserves this 4 KiB (tool_execute 4,480 B,
     * ctx_allow 4,192 B frames) although only an ASK or inside-check fills
     * it. perm_req_init() stopped the zeroing; moving resolved to a caller
     * buffer taken on that path would stop the reservation too.
     */
    char resolved[PATH_MAX];
} PermReq;

/*
 * One place that must learn about a new PermReq field. The caller sets the
 * resource pointer after this; resolved is an output and starts empty.
 */
static inline void perm_req_init(PermReq *r, enum ToolAction action,
                                 const char *tool) {
    r->action = action;
    r->tool = tool;
    r->path = NULL;
    r->command = NULL;
    r->preview = NULL;
    r->preview_len = 0;
    r->resolved[0] = '\0';
}

/*
 * A self-contained ask record: it owns its bytes and survives the JDoc free.
 * path is the resolved absolute path when resolution succeeded, else the raw
 * argument. text is the command or the bounded mutation preview.
 */
typedef struct {
    enum ToolAction action;
    enum ToolAskKind kind;
    char tool[32];
    char path[PATH_MAX];
    char text[512];
    size_t text_len;
    int text_truncated;
} ToolAsk;

typedef struct {
    const char *cwd;
    unsigned perms;
    int call_approved; /* set only while re-running the approved call */
} PermPolicy;

enum {
    PERM_YOLO_READ = 1u << 0,
    PERM_ALLOW_WRITES = 1u << 1,
    PERM_ALLOW_EXEC = 1u << 2,
    PERM_SESSION_READ = 1u << 3,
    PERM_SESSION_MUTATE = 1u << 4,
    PERM_SESSION_EXEC = 1u << 5,
};

/* 1 when resolved is a real path, 0 when the leaf or a parent does not exist. */
int perm_resolve_path(const char *cwd, const char *path, char *out, size_t cap);

/* Prefix compare with a '/' or NUL boundary; resolved must be canonical. */
int perm_path_within_cwd(const char *cwd, const char *resolved);

enum PermDecision perm_decide(const PermPolicy *p, PermReq *r);

void perm_fill_ask(ToolAsk *ask, const PermReq *req);
void perm_ask_clear(ToolAsk *ask);

#endif
