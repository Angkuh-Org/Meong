#include <stdio.h>

#include "rules/rulebook.h"

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "src";
    return meong_rules_audit(root, stderr) == 0 ? 0 : 1;
}
