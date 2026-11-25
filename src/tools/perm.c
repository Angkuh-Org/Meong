#include "tools/perm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

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
 * treated as outside, never as inside. A trailing slash is stripped before the
 * resolve so an exact deny match cannot be defeated by `hardlink/`.
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
    size_t alen = (size_t)n;
    while (alen > 1 && abs[alen - 1] == '/') {
        abs[--alen] = '\0';
    }
    if (strpbrk(abs, "\n\r")) {
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

static int basename_is(const char *b, const char *lit) {
    return strcasecmp(b, lit) == 0;
}

typedef struct {
    const char *s;
    size_t n;
} Lit;

#define LIT(s) { (s), sizeof(s) - 1 }

/* Length travels with the literal: the component/root loops compare against
 * every entry, so a strlen() per entry is pure overhead. */
static int lit_is(const char *s, size_t n, const Lit *lit) {
    return n == lit->n && strncasecmp(s, lit->s, n) == 0;
}

static int prefix_is(const char *path, const Lit *prefix) {
    return strncmp(path, prefix->s, prefix->n) == 0 &&
           (path[prefix->n] == '/' || path[prefix->n] == '\0');
}

/*
 * The unwaivable list. Component names catch anything below a credential or
 * VCS directory; the basename set catches drop-in secrets. `--yolo`,
 * `--allow-writes`, a session bit and one-call approval all run after this.
 */
int perm_protected(const char *resolved) {
    if (!resolved || resolved[0] != '/') {
        return 0;
    }
    static const Lit components[] = {
        LIT(".git"), LIT(".ssh"), LIT(".gnupg"),
        LIT(".paig"), LIT(".aws"), LIT(".kube"),
    };
    for (const char *p = resolved; *p;) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        for (size_t i = 0; i < sizeof components / sizeof components[0]; i++) {
            if (lit_is(p, n, &components[i])) {
                return 1;
            }
        }
        p = e ? e : p + n;
    }

    const char *b = strrchr(resolved, '/');
    b = b ? b + 1 : resolved;
    if (basename_is(b, ".env")) {
        return 1;
    }
    if (strncasecmp(b, ".env.", 5) == 0) {
        static const char *ok[] = {
            ".env.example", ".env.sample", ".env.template", ".env.dist",
        };
        for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
            if (basename_is(b, ok[i])) {
                return 0;
            }
        }
        return 1;
    }
    static const char *basenames[] = {
        "authorized_keys", ".netrc", ".bashrc", ".bash_profile",
        ".zshrc", ".profile",
    };
    for (size_t i = 0; i < sizeof basenames / sizeof basenames[0]; i++) {
        if (basename_is(b, basenames[i])) {
            return 1;
        }
    }
    if (strcasecmp(b, "config.fish") == 0) {
        const char *fish = strstr(resolved, "/.config/fish/");
        if (fish && fish + strlen("/.config/fish/") == b) {
            return 1;
        }
    }

    if (geteuid() == 0) {
        static const Lit roots[] = {
            LIT("/data/adb"), LIT("/data/system"), LIT("/data/misc"),
            LIT("/system"), LIT("/vendor"), LIT("/product"), LIT("/apex"),
        };
        for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
            if (prefix_is(resolved, &roots[i])) {
                return 1;
            }
        }
    }
    return 0;
}

/* Lexically normalise an absolute or cwd-relative path into out. */
static int normalize_abs(const char *cwd, const char *path, char *out,
                         size_t cap) {
    char abs[PATH_MAX];
    int n;
    if (path[0] == '/') {
        n = snprintf(abs, sizeof abs, "%s", path);
    } else {
        if (!cwd || !*cwd) {
            errno = EINVAL;
            return -1;
        }
        n = snprintf(abs, sizeof abs, "%s/%s", cwd, path);
    }
    if (n < 0 || (size_t)n >= sizeof abs) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (strpbrk(abs, "\n\r")) {
        errno = EINVAL;
        return -1;
    }
    size_t o = 0;
    if (cap < 2) {
        errno = ENAMETOOLONG;
        return -1;
    }
    out[o++] = '/';
    out[o] = '\0';
    for (const char *p = abs; *p;) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 1 && p[0] == '.') {
            p += len;
            continue;
        }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            size_t k = o;
            while (k > 1 && out[k - 1] != '/') {
                k--;
            }
            if (k > 1) {
                o = k - 1;
                out[o] = '\0';
            } else {
                o = 1;
                out[1] = '\0';
            }
            p += len;
            continue;
        }
        if (len > NAME_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (o > 1) {
            if (o + 1 >= cap) {
                errno = ENAMETOOLONG;
                return -1;
            }
            out[o++] = '/';
        }
        if (o + len + 1 > cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(out + o, p, len);
        o += len;
        out[o] = '\0';
        p += len;
    }
    return 0;
}

/*
 * Mutation resolver. The cwd is already canonical, so opening it with
 * O_NOFOLLOW is safe; every later component is opened relative to the previous
 * dirfd, which pins the inode and makes a concurrent rename unable to redirect
 * the walk. `..` is collapsed lexically before the walk, so a component never
 * leaves the path the human sees.
 */
int perm_open_target(const char *cwd, const char *path, char *resolved,
                     size_t cap, int *dfd_out, const char **base_out) {
    if (!resolved || cap == 0 || !dfd_out || !base_out || !path || !*path) {
        errno = EINVAL;
        return -1;
    }
    if (normalize_abs(cwd, path, resolved, cap) != 0) {
        return -1;
    }
    if (strcmp(resolved, "/") == 0) {
        errno = EISDIR;
        return -1;
    }
    char *slash = strrchr(resolved, '/');
    if (!slash || !slash[1]) {
        errno = EINVAL;
        return -1;
    }
    const char *base = slash + 1;

    char parent[PATH_MAX];
    size_t plen = (size_t)(slash - resolved);
    if (plen == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        if (plen >= sizeof parent) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(parent, resolved, plen);
        parent[plen] = '\0';
    }

    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    char *p = parent + 1;
    while (*p) {
        char *e = strchr(p, '/');
        if (e) {
            *e = '\0';
        }
        int next = openat(fd, p, O_PATH | O_DIRECTORY | O_NOFOLLOW |
                                     O_CLOEXEC);
        if (next < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        close(fd);
        fd = next;
        if (!e) {
            break;
        }
        p = e + 1;
    }
    *dfd_out = fd;
    *base_out = base;
    return 0;
}

enum PermDecision perm_decide(const PermPolicy *p, PermReq *r) {
    if (!p || !r) {
        return PERM_DENY;
    }
    if (r->action == TOOL_ACT_READ && (!r->path || !*r->path)) {
        return PERM_DENY;
    }
    if (r->action == TOOL_ACT_MUTATE && (!r->path || !*r->path)) {
        return PERM_DENY;
    }
    if (r->action == TOOL_ACT_EXEC && (!r->command || !*r->command)) {
        return PERM_DENY;
    }

    /*
     * Resolve once. tool_execute has already filled this for the top-level
     * call; ctx_allow leaves it empty and gets the per-file resolve. An
     * unresolved path stays outside and is never an allow.
     */
    if (!r->resolved[0] && r->path && *r->path) {
        perm_resolve_path(p->cwd, r->path, r->resolved, sizeof r->resolved);
    }
    if (r->resolved[0] && perm_protected(r->resolved)) {
        return PERM_DENY_PROTECTED;
    }

    if (r->action == TOOL_ACT_MUTATE && (p->perms & PERM_NO_MUTATE)) {
        return PERM_DENY_MUTATE;
    }

    if (p->call_approved) {
        if (r->action == TOOL_ACT_MUTATE) {
            /* Fail closed: a mutation approval without a ticket is not an
             * approval. The ticket is the bytes the human saw. */
            if (!p->approved || !p->approved->valid || !r->ident ||
                !r->ident->valid) {
                return PERM_DENY;
            }
            if (p->approved->kind != r->ident->kind ||
                p->approved->dev != r->ident->dev ||
                p->approved->ino != r->ident->ino ||
                p->approved->size != r->ident->size ||
                p->approved->mtime_s != r->ident->mtime_s ||
                p->approved->mtime_ns != r->ident->mtime_ns ||
                p->approved->mode != r->ident->mode ||
                p->approved->hash != r->ident->hash) {
                return PERM_STALE;
            }
        }
        return PERM_ALLOW;
    }

    switch (r->action) {
    case TOOL_ACT_READ:
        if (p->perms & (PERM_YOLO_READ | PERM_SESSION_READ)) {
            return PERM_ALLOW;
        }
        return perm_path_within_cwd(p->cwd, r->resolved) ? PERM_ALLOW
                                                         : PERM_ASK;
    case TOOL_ACT_MUTATE:
        if (p->perms & (PERM_ALLOW_WRITES | PERM_SESSION_MUTATE)) {
            return PERM_ALLOW;
        }
        return PERM_ASK;
    case TOOL_ACT_EXEC:
        if (p->perms & (PERM_ALLOW_EXEC | PERM_SESSION_EXEC)) {
            return PERM_ALLOW;
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
    if (req->ident) {
        ask->ident = *req->ident;
    }
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
