#ifndef MEONG_RULEBOOK_H
#define MEONG_RULEBOOK_H

#include <stdio.h>

/*
 * The rulebook is the single source of truth for project law.  The build
 * compiles a host-side rulecheck from this table, so a rule that is not
 * here does not exist.  Each entry needs a matching check in
 * meong_rules_audit().
 */
typedef struct {
    const char *id;
    const char *law;
    long limit;
} MeongRule;

extern const MeongRule MEONG_RULEBOOK[];
extern const int MEONG_RULE_COUNT;

/* Returns the number of violations; prints them to report. */
int meong_rules_audit(const char *root, FILE *report);

#endif
