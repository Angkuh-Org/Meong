#include "rules/rulebook.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const MeongRule MEONG_RULEBOOK[] = {
    {
        .id = "loc-per-file",
        .law = "no .c or .h under src/ may exceed this many physical lines",
        .limit = 2000,
    },
};
const int MEONG_RULE_COUNT =
    (int)(sizeof MEONG_RULEBOOK / sizeof MEONG_RULEBOOK[0]);

static const MeongRule *rule_find(const char *id) {
    for (int i = 0; i < MEONG_RULE_COUNT; i++) {
        if (strcmp(MEONG_RULEBOOK[i].id, id) == 0) {
            return &MEONG_RULEBOOK[i];
        }
    }
    return NULL;
}

static char *path_join(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    char *p = malloc(dl + nl + 2);
    if (!p) {
        fputs("rulebook: out of memory\n", stderr);
        exit(1);
    }
    memcpy(p, dir, dl);
    p[dl] = '/';
    memcpy(p + dl + 1, name, nl + 1);
    return p;
}

static long count_lines(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    long lines = 0;
    int last = '\n';
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            lines++;
        }
        last = c;
    }
    fclose(f);
    if (last != '\n') {
        lines++;
    }
    return lines;
}

static int is_source(const char *name) {
    size_t n = strlen(name);
    return n >= 2 && name[n - 2] == '.' &&
           (name[n - 1] == 'c' || name[n - 1] == 'h');
}

static int audit_dir(const char *dir, const MeongRule *rule, FILE *report,
                     int *checked) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(report, "rulebook: cannot open %s: %s\n", dir, strerror(errno));
        return 1;
    }
    int bad = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char *path = path_join(dir, ent->d_name);
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                bad += audit_dir(path, rule, report, checked);
            } else if (S_ISREG(st.st_mode) && is_source(ent->d_name)) {
                (*checked)++;
                long lines = count_lines(path);
                if (lines < 0) {
                    fprintf(report, "rulebook: cannot read %s\n", path);
                    bad++;
                } else if (lines > rule->limit) {
                    fprintf(report, "%s: %ld lines > %ld (%s)\n", path, lines,
                            rule->limit, rule->id);
                    bad++;
                }
            }
        }
        free(path);
    }
    closedir(d);
    return bad;
}

int meong_rules_audit(const char *root, FILE *report) {
    if (!root || !*root) {
        root = "src";
    }
    if (!report) {
        report = stderr;
    }
    const MeongRule *rule = rule_find("loc-per-file");
    if (!rule) {
        fputs("rulebook: loc-per-file rule is missing\n", report);
        return 1;
    }
    int checked = 0;
    int bad = audit_dir(root, rule, report, &checked);
    if (bad == 0) {
        fprintf(report, "rulebook: %d source files ok (%s: <= %ld lines)\n",
                checked, rule->id, rule->limit);
    } else {
        fprintf(report, "rulebook: %d violation(s) across %d source files\n",
                bad, checked);
    }
    return bad;
}
