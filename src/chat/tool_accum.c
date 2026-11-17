/*
 * Streamed tool-call accumulation.  cc sends whole calls (whole=1); OpenAI
 * compatible streams send argument fragments (whole=0).  Each used slot owns
 * its id/name/args; clear/reset free whatever the caller has not moved out.
 */
#include "chat/tool_accum.h"

#include <stdlib.h>
#include <string.h>

static void slot_free(ToolCall *c) {
    free(c->id);
    free(c->name);
    free(c->args);
    memset(c, 0, sizeof *c);
}

static char *dup_bytes(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static int find_id(const ToolAccum *ta, const char *id, size_t id_len) {
    for (int i = 0; i < TOOL_MAX_CALLS; i++) {
        if (!ta->used[i] || !ta->calls[i].id) {
            continue;
        }
        if (strlen(ta->calls[i].id) == id_len &&
            memcmp(ta->calls[i].id, id, id_len) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free(const ToolAccum *ta) {
    for (int i = 0; i < TOOL_MAX_CALLS; i++) {
        if (!ta->used[i]) {
            return i;
        }
    }
    return -1;
}

static int find_last_used(const ToolAccum *ta) {
    for (int i = TOOL_MAX_CALLS - 1; i >= 0; i--) {
        if (ta->used[i]) {
            return i;
        }
    }
    return -1;
}

void tool_accum_clear(ToolAccum *ta) {
    for (int i = 0; i < TOOL_MAX_CALLS; i++) {
        if (ta->used[i]) {
            slot_free(&ta->calls[i]);
        }
    }
    memset(ta->calls, 0, sizeof ta->calls);
    memset(ta->used, 0, sizeof ta->used);
    ta->ncalls = 0;
}

/*
 * The caller must zero the ToolAccum before the first reset; every later
 * reset frees whatever the previous generation left behind.
 */
void tool_accum_reset(ToolAccum *ta, int whole) {
    tool_accum_clear(ta);
    ta->whole = whole != 0;
}

int tool_accum_add(ToolAccum *ta, int has_index, int index,
                   const char *id, size_t id_len,
                   const char *name, size_t name_len,
                   const char *args, size_t args_len) {
    if (!id) {
        id_len = 0;
    }
    if (!name) {
        name_len = 0;
    }
    if (!args) {
        args_len = 0;
    }

    int slot;
    if (has_index) {
        if (index < 0 || index >= TOOL_MAX_CALLS) {
            return -1;
        }
        slot = index;
    } else if (id_len > 0) {
        slot = find_id(ta, id, id_len);
        if (slot < 0) {
            slot = find_free(ta);
        }
    } else if (!ta->whole && name_len == 0 && args_len > 0) {
        /* A nameless, id-less, index-less fragment continues the last call. */
        slot = find_last_used(ta);
        if (slot < 0) {
            slot = find_free(ta);
        }
    } else {
        slot = find_free(ta);
    }
    if (slot < 0) {
        return -1;
    }

    ToolCall *c = &ta->calls[slot];

    if (ta->whole) {
        if (args_len > TOOL_MAX_ARGS) {
            return -1;
        }
        if (id_len > 0) {
            char *p = dup_bytes(id, id_len);
            free(c->id);
            c->id = p;
        }
        if (name_len > 0) {
            char *p = dup_bytes(name, name_len);
            free(c->name);
            c->name = p;
        }
        if (args_len > 0) {
            char *p = dup_bytes(args, args_len);
            free(c->args);
            c->args = p;
            c->args_len = args_len;
            c->args_cap = args_len;
        }
    } else {
        if (c->args_len > TOOL_MAX_ARGS ||
            args_len > TOOL_MAX_ARGS - c->args_len) {
            return -1;
        }
        if (id_len > 0) {
            char *p = dup_bytes(id, id_len);
            free(c->id);
            c->id = p;
        }
        if (name_len > 0) {
            char *p = dup_bytes(name, name_len);
            free(c->name);
            c->name = p;
        }
        size_t len = c->args_len + args_len;
        if (len > 0) {
            if (len > c->args_cap) {
                size_t cap = c->args_cap ? c->args_cap : 32;
                while (cap < len) {
                    cap += cap / 2;
                }
                if (cap > TOOL_MAX_ARGS) {
                    cap = TOOL_MAX_ARGS;
                }
                c->args = xrealloc(c->args, cap + 1);
                c->args_cap = cap;
            }
            if (args_len > 0) {
                memcpy(c->args + c->args_len, args, args_len);
            }
            c->args[len] = '\0';
            c->args_len = len;
        }
    }

    ta->used[slot] = 1;
    if (slot >= ta->ncalls) {
        ta->ncalls = slot + 1;
    }
    return 0;
}
