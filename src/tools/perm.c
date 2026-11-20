#include "tools/perm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Resolve abs into out. cap >= PATH_MAX writes straight into the caller's
 * buffer; a smaller buffer falls back to a heap path so the function itself
 * never carries a second PATH_MAX frame.
 */
static int resolve_into(const char *abs, char *out, size_t cap) {
    if (cap >= PATH_MAX) {
        if (realpath(abs, out)) {
            return 1;
        }
        out[0] = '\0';
        return 0;
    }
    char *real = realpath(abs, NULL);
    if (!real) {
        out[0] = '\0';
        return 0;
    }
    int n = snprintf(out, cap, "%s", real);
    free(real);
    if (n > 0 && (size_t)n < cap) {
        return 1;
    }
    out[0] = '\0';
    return 0;
}

/*
 * Resolve a tool path against the cwd. Existing paths use realpath; for a
 * missing leaf, resolve the parent and append the basename. If the parent does
 * not resolve either, return 0 and leave out empty: an unresolved path is
 * treated as outside, never as inside.
 */
int perm_resolve_path(const char *cwd, const char *path, char *out,
                      size_t cap) {
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!path || !*path) {
        return 0;
    }
    char abs[PATH_MAX];
    int n;
    if (path[0] == '/') {
        n = snprintf(abs, sizeof abs, "%s", path);
    } else {
        if (!cwd || !*cwd) {
            return 0;
        }
        n = snprintf(abs, sizeof abs, "%s/%s", cwd, path);
    }
    if (n < 0 || (size_t)n >= sizeof abs) {
        return 0;
    }

    if (resolve_into(abs, out, cap)) {
        return 1;
    }

    char *slash = strrchr(abs, '/');
    if (!slash) {
        return 0;
    }
    /* base points past the NUL we write below; its bytes stay in abs. */
    const char *base = slash + 1;
    if (slash == abs) {
        abs[1] = '\0';
    } else {
        *slash = '\0';
    }
    if (!resolve_into(abs, out, cap)) {
        return 0;
    }
    size_t plen = strlen(out);
    n = snprintf(out + plen, cap - plen, "/%s", base);
    return n > 0 && (size_t)n < cap - plen;
}

int perm_path_within_cwd(const char *cwd, const char *resolved) {
    if (!cwd || !*cwd || !resolved || !*resolved) {
        return 0;
    }
    if (strcmp(cwd, "/") == 0) {
        return resolved[0] == '/';
    }
    size_t n = strlen(cwd);
    return strncmp(resolved, cwd, n) == 0 &&
           (resolved[n] == '/' || resolved[n] == '\0');
}

enum PermDecision perm_decide(const PermPolicy *p, PermReq *r) {
    if (!p || !r) {
        return PERM_DENY;
    }
    r->resolved[0] = '\0';
    /*
     * Fast paths decide before realpath: a search gate runs once per file,
     * and a session/yolo/approved allow needs no canonical path.
     */
    if (p->call_approved) {
        return PERM_ALLOW;
    }
    switch (r->action) {
    case TOOL_ACT_READ:
        if (!r->path || !*r->path) {
            return PERM_DENY;
        }
        if (p->perms & (PERM_YOLO_READ | PERM_SESSION_READ)) {
            return PERM_ALLOW;
        }
        perm_resolve_path(p->cwd, r->path, r->resolved, sizeof r->resolved);
        return perm_path_within_cwd(p->cwd, r->resolved) ? PERM_ALLOW
                                                         : PERM_ASK;
    case TOOL_ACT_MUTATE:
        if (!r->path || !*r->path) {
            return PERM_DENY;
        }
        if (p->perms & (PERM_ALLOW_WRITES | PERM_SESSION_MUTATE)) {
            return PERM_ALLOW;
        }
        perm_resolve_path(p->cwd, r->path, r->resolved, sizeof r->resolved);
        return PERM_ASK;
    case TOOL_ACT_EXEC:
        if (!r->command || !*r->command) {
            return PERM_DENY;
        }
        if (p->perms & (PERM_ALLOW_EXEC | PERM_SESSION_EXEC)) {
            return PERM_ALLOW;
        }
        if (r->path && *r->path) {
            perm_resolve_path(p->cwd, r->path, r->resolved,
                              sizeof r->resolved);
        }
        return PERM_ASK;
    default:
        return PERM_DENY;
    }
}

void perm_ask_clear(ToolAsk *ask) {
    if (ask) {
        memset(ask, 0, sizeof *ask);
    }
}

void perm_fill_ask(ToolAsk *ask, const PermReq *req) {
    if (!ask || !req) {
        return;
    }
    memset(ask, 0, sizeof *ask);
    ask->action = req->action;
    snprintf(ask->tool, sizeof ask->tool, "%s", req->tool ? req->tool : "tool");
    snprintf(ask->path, sizeof ask->path, "%s",
             req->resolved[0] ? req->resolved
                              : (req->path ? req->path : ""));
    if (req->command) {
        ask->kind = TOOL_ASK_COMMAND;
    } else if (req->preview) {
        ask->kind = TOOL_ASK_MUTATE;
    } else {
        ask->kind = TOOL_ASK_PATH;
    }
    const char *text = req->command ? req->command : req->preview;
    if (!text) {
        return;
    }
    size_t full = req->command ? strlen(req->command) : req->preview_len;
    ask->text_len = full;
    size_t n = full;
    if (n >= sizeof ask->text) {
        n = sizeof ask->text - 1;
        ask->text_truncated = 1;
    }
    memcpy(ask->text, text, n);
    ask->text[n] = '\0';
}
