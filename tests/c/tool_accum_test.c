/*
 * tool_accum owns the streamed tool_calls[] slots: whole replacements for
 * cc, raw fragment appends for OpenAI streaming, sparse indices, id reuse,
 * and the id-less "new slot, never merge" rule.
 */
#include "chat/tool_accum.h"

#include <stdio.h>
#include <string.h>

static int fails;
static int checks;

static void check(int cond, const char *what) {
    checks++;
    if (cond) {
        printf("ok %d - %s\n", checks, what);
    } else {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

static int args_eq(const ToolCall *c, const char *want) {
    size_t n = strlen(want);
    return c->args && c->args_len == n && memcmp(c->args, want, n) == 0 &&
           c->args[n] == '\0';
}

static void test_whole(void) {
    ToolAccum ta;
    memset(&ta, 0, sizeof ta);
    tool_accum_reset(&ta, 1);

    const char *a0 = "{\"path\":\"a\"}";
    const char *a1 = "{\"path\":\"b\",\"data\":\"x\"}";
    check(ta.whole == 1, "reset(1) selects whole calls");
    check(ta.ncalls == 0, "reset starts empty");
    check(tool_accum_add(&ta, 1, 0, "call_a", strlen("call_a"), "read_file",
                         strlen("read_file"), a0, strlen(a0)) == 0,
          "whole call at index 0 accepted");
    check(tool_accum_add(&ta, 1, 1, "call_b", strlen("call_b"), "write_file",
                         strlen("write_file"), a1, strlen(a1)) == 0,
          "whole call at index 1 accepted");
    check(ta.ncalls == 2, "whole: ncalls is 2");
    check(ta.used[0] && ta.used[1], "whole: both slots are used");
    check(ta.calls[0].id && strcmp(ta.calls[0].id, "call_a") == 0,
          "whole: slot 0 id");
    check(ta.calls[0].name && strcmp(ta.calls[0].name, "read_file") == 0,
          "whole: slot 0 name");
    check(args_eq(&ta.calls[0], a0), "whole: slot 0 args");
    check(ta.calls[1].id && strcmp(ta.calls[1].id, "call_b") == 0,
          "whole: slot 1 id");
    check(ta.calls[1].name && strcmp(ta.calls[1].name, "write_file") == 0,
          "whole: slot 1 name");
    check(args_eq(&ta.calls[1], a1), "whole: slot 1 args");

    const char *a0b = "{\"path\":\"c\"}";
    check(tool_accum_add(&ta, 1, 0, "call_a2", strlen("call_a2"),
                         "list_dir", strlen("list_dir"), NULL, 0) == 0,
          "whole: id/name replacement accepted");
    check(ta.calls[0].id && strcmp(ta.calls[0].id, "call_a2") == 0,
          "whole: id replaced");
    check(ta.calls[0].name && strcmp(ta.calls[0].name, "list_dir") == 0,
          "whole: name replaced");
    check(args_eq(&ta.calls[0], a0), "whole: empty args keep the old args");
    check(tool_accum_add(&ta, 1, 0, NULL, 0, NULL, 0, a0b, strlen(a0b)) == 0,
          "whole: args replacement accepted");
    check(args_eq(&ta.calls[0], a0b), "whole: args replaced, not appended");
    check(ta.calls[0].name && strcmp(ta.calls[0].name, "list_dir") == 0,
          "whole: empty name keeps the old one");

    const char *b2 = "{\"path\":\"d\"}";
    check(tool_accum_add(&ta, 0, 0, "call_b", strlen("call_b"), NULL, 0, b2,
                         strlen(b2)) == 0,
          "whole: id reuse without an index");
    check(ta.ncalls == 2, "whole: id reuse keeps ncalls");
    check(args_eq(&ta.calls[1], b2), "whole: id reuse replaces slot 1 args");
    check(!ta.used[2], "whole: id reuse did not take a new slot");
    tool_accum_clear(&ta);
}

static void test_fragments(void) {
    ToolAccum ta;
    memset(&ta, 0, sizeof ta);
    tool_accum_reset(&ta, 0);
    check(ta.whole == 0, "reset(0) selects fragments");
    check(tool_accum_add(&ta, 1, 0, "call_f", strlen("call_f"), "read_file",
                         strlen("read_file"), "{\"pa", 4) == 0,
          "fragment 1 accepted");
    check(tool_accum_add(&ta, 1, 0, NULL, 0, NULL, 0, "th\":\"x", 6) == 0,
          "fragment 2 accepted");
    check(tool_accum_add(&ta, 1, 0, NULL, 0, NULL, 0, "\"}", 2) == 0,
          "fragment 3 accepted");
    check(ta.ncalls == 1, "fragments share one slot");
    check(ta.used[0] && !ta.used[1], "fragments only use slot 0");
    check(ta.calls[0].id && strcmp(ta.calls[0].id, "call_f") == 0,
          "fragments keep the id");
    check(ta.calls[0].name && strcmp(ta.calls[0].name, "read_file") == 0,
          "fragments keep the name");
    check(args_eq(&ta.calls[0], "{\"path\":\"x\"}"),
          "fragments concatenate to the final args");
    tool_accum_clear(&ta);
}

static void test_fragment_continuation(void) {
    ToolAccum ta;
    memset(&ta, 0, sizeof ta);
    tool_accum_reset(&ta, 0);
    check(tool_accum_add(&ta, 1, 0, "call_1", strlen("call_1"), "read_file",
                         strlen("read_file"), "{\"path\":", 8) == 0,
          "continuation: first frame accepted");
    check(tool_accum_add(&ta, 0, 0, NULL, 0, NULL, 0, "\"/proj/", 7) == 0,
          "continuation: nameless frame 2 accepted");
    check(tool_accum_add(&ta, 0, 0, NULL, 0, NULL, 0, "version.txt\"}", 13) ==
              0,
          "continuation: nameless frame 3 accepted");
    check(ta.ncalls == 1, "continuation: all frames share one slot");
    check(ta.used[0] && !ta.used[1], "continuation: no empty-id extra slot");
    check(ta.calls[0].id && strcmp(ta.calls[0].id, "call_1") == 0,
          "continuation: id survives");
    check(ta.calls[0].name && strcmp(ta.calls[0].name, "read_file") == 0,
          "continuation: name survives");
    check(args_eq(&ta.calls[0], "{\"path\":\"/proj/version.txt\"}"),
          "continuation: args are the full concatenation");

    check(tool_accum_add(&ta, 0, 0, NULL, 0, "list_dir", strlen("list_dir"),
                         "{}", 2) == 0,
          "continuation: named id-less call accepted");
    check(ta.ncalls == 2 && ta.used[1],
          "continuation: a name still starts a new slot");
    check(args_eq(&ta.calls[0], "{\"path\":\"/proj/version.txt\"}"),
          "continuation: the new call did not touch slot 0");
    tool_accum_clear(&ta);

    tool_accum_reset(&ta, 0);
    check(tool_accum_add(&ta, 0, 0, NULL, 0, NULL, 0, "{}", 2) == 0,
          "continuation: first nameless fragment accepted");
    check(ta.ncalls == 1 && ta.used[0],
          "continuation: no used slot still starts slot 0");
    check(args_eq(&ta.calls[0], "{}"),
          "continuation: first nameless fragment keeps its args");
    tool_accum_clear(&ta);

    tool_accum_reset(&ta, 1);
    check(tool_accum_add(&ta, 1, 0, "call_w", strlen("call_w"), "read_file",
                         strlen("read_file"), "{\"path\":\"a\"}", 12) == 0,
          "continuation: whole-mode call prepared");
    check(tool_accum_add(&ta, 0, 0, NULL, 0, NULL, 0, "\"/proj/", 7) == 0,
          "continuation: whole-mode nameless frame accepted");
    check(ta.ncalls == 2 && ta.used[1],
          "continuation: whole mode starts a new slot");
    check(args_eq(&ta.calls[0], "{\"path\":\"a\"}"),
          "continuation: whole mode keeps the first call intact");
    tool_accum_clear(&ta);
}

static void test_sparse(void) {
    ToolAccum ta;
    memset(&ta, 0, sizeof ta);
    tool_accum_reset(&ta, 1);
    check(tool_accum_add(&ta, 1, 2, "call_c", strlen("call_c"), "list_dir",
                         strlen("list_dir"), "{}", 2) == 0,
          "sparse index 2 accepted");
    check(ta.ncalls == 3, "sparse: ncalls is highest index + 1");
    check(!ta.used[0] && !ta.used[1] && ta.used[2],
          "sparse: only slot 2 is used");
    check(ta.calls[0].id == NULL && ta.calls[0].name == NULL &&
              ta.calls[0].args == NULL && ta.calls[0].args_len == 0,
          "sparse: slot 0 stays zeroed");
    check(ta.calls[1].id == NULL && ta.calls[1].name == NULL &&
              ta.calls[1].args == NULL && ta.calls[1].args_len == 0,
          "sparse: slot 1 stays zeroed");
    check(args_eq(&ta.calls[2], "{}"), "sparse: slot 2 args");
    tool_accum_clear(&ta);
}

static void test_id_reuse(void) {
    ToolAccum ta;
    memset(&ta, 0, sizeof ta);
    tool_accum_reset(&ta, 0);
    check(tool_accum_add(&ta, 1, 0, "call_a", strlen("call_a"), "read_file",
                         strlen("read_file"), "{\"a\":", 5) == 0,
          "id reuse: slot 0 first fragment");
    check(tool_accum_add(&ta, 1, 1, "call_b", strlen("call_b"), "list_dir",
                         strlen("list_dir"), "{}", 2) == 0,
          "id reuse: slot 1 call");
    check(tool_accum_add(&ta, 0, 0, "call_a", strlen("call_a"), NULL, 0, "1}",
                         2) == 0,
          "id reuse: no index, matching id");
    check(ta.ncalls == 2, "id reuse: matching id adds no slot");
    check(ta.used[0] && ta.used[1] && !ta.used[2],
          "id reuse: slots 0/1 used, slot 2 free");
    check(args_eq(&ta.calls[0], "{\"a\":1}"), "id reuse: slot 0 appended");
    check(args_eq(&ta.calls[1], "{}"), "id reuse: slot 1 untouched");
    check(tool_accum_add(&ta, 0, 0, "call_c", strlen("call_c"), "read_file",
                         strlen("read_file"), "{}", 2) == 0,
          "id reuse: unknown id accepted");
    check(ta.ncalls == 3 && ta.used[2],
          "id reuse: unknown id takes the next free slot");
    tool_accum_clear(&ta);
}

static void test_idless(void) {
    ToolAccum ta;
    memset(&ta, 0, sizeof ta);
    tool_accum_reset(&ta, 0);
    check(tool_accum_add(&ta, 0, 0, NULL, 0, "read_file",
                         strlen("read_file"), "{\"a\":1}", 7) == 0,
          "id-less: first call accepted");
    check(tool_accum_add(&ta, 0, 0, NULL, 0, "read_file",
                         strlen("read_file"), "{\"b\":2}", 7) == 0,
          "id-less: second call accepted");
    check(ta.ncalls == 2, "id-less: two calls take two slots");
    check(ta.used[0] && ta.used[1], "id-less: both slots are used");
    check(args_eq(&ta.calls[0], "{\"a\":1}"),
          "id-less: slot 0 keeps only its args");
    check(args_eq(&ta.calls[1], "{\"b\":2}"),
          "id-less: slot 1 keeps only its args");
    tool_accum_clear(&ta);
}

static void test_empty_args(void) {
    ToolAccum ta;
    memset(&ta, 0, sizeof ta);
    tool_accum_reset(&ta, 1);
    check(tool_accum_add(&ta, 1, 0, "call_e", strlen("call_e"), "list_dir",
                         strlen("list_dir"), NULL, 0) == 0,
          "empty args: call accepted");
    check(ta.used[0] && ta.calls[0].args == NULL &&
              ta.calls[0].args_len == 0,
          "empty args: NULL/0 lets callers emit {}");
    check(tool_accum_add(&ta, 1, 0, NULL, 0, NULL, 0, "", 0) == 0,
          "empty args: empty string is a no-op");
    check(ta.calls[0].args == NULL && ta.calls[0].args_len == 0,
          "empty args: still NULL/0");
    tool_accum_clear(&ta);
}

static void test_clear_reset(void) {
    ToolAccum ta;
    memset(&ta, 0, sizeof ta);
    tool_accum_reset(&ta, 1);
    check(tool_accum_add(&ta, 1, 0, "call_a", strlen("call_a"), "read_file",
                         strlen("read_file"), "{}", 2) == 0,
          "clear: slot 0 prepared");
    check(tool_accum_add(&ta, 1, 1, "call_b", strlen("call_b"), "list_dir",
                         strlen("list_dir"), "{}", 2) == 0,
          "clear: slot 1 prepared");
    tool_accum_clear(&ta);
    check(ta.ncalls == 0, "clear resets ncalls");
    check(!ta.used[0] && !ta.used[1], "clear resets used bits");
    check(ta.calls[0].id == NULL && ta.calls[0].name == NULL &&
              ta.calls[0].args == NULL && ta.calls[0].args_len == 0,
          "clear frees and zeroes slot 0");
    check(ta.calls[1].id == NULL && ta.calls[1].name == NULL &&
              ta.calls[1].args == NULL && ta.calls[1].args_len == 0,
          "clear frees and zeroes slot 1");
    tool_accum_clear(&ta);
    check(ta.ncalls == 0 && !ta.used[0], "clear is safe to call twice");

    tool_accum_reset(&ta, 0);
    check(ta.whole == 0, "reset switches whole off");
    check(ta.ncalls == 0, "reset leaves no calls");
    tool_accum_reset(&ta, 1);
    check(ta.whole == 1, "reset switches whole on");
    tool_accum_clear(&ta);
}

static void test_overflow(void) {
    ToolAccum ta;
    memset(&ta, 0, sizeof ta);
    tool_accum_reset(&ta, 0);

    static char big[TOOL_MAX_ARGS + 1];
    memset(big, 'x', sizeof big);

    check(tool_accum_add(&ta, 1, 0, "call_big", strlen("call_big"),
                         "read_file", strlen("read_file"), big,
                         TOOL_MAX_ARGS) == 0,
          "overflow: exactly TOOL_MAX_ARGS accepted");
    check(ta.calls[0].args_len == TOOL_MAX_ARGS,
          "overflow: max args length is exact");
    check(ta.calls[0].args && ta.calls[0].args[TOOL_MAX_ARGS] == '\0',
          "overflow: max args stay NUL-terminated");

    check(tool_accum_add(&ta, 1, 0, NULL, 0, NULL, 0, "y", 1) == -1,
          "overflow: one byte past TOOL_MAX_ARGS rejected");
    check(ta.calls[0].args_len == TOOL_MAX_ARGS,
          "overflow: rejected append leaves args_len intact");
    check(ta.calls[0].args && ta.calls[0].args[TOOL_MAX_ARGS] == '\0',
          "overflow: rejected append leaves args intact");
    check(tool_accum_add(&ta, 1, 1, "call_ok", strlen("call_ok"),
                         "read_file", strlen("read_file"), "{}", 2) == 0,
          "overflow: accumulator is still usable");
    check(ta.ncalls == 2, "overflow: next slot still allocates");
    tool_accum_clear(&ta);

    tool_accum_reset(&ta, 1);
    check(tool_accum_add(&ta, 1, 0, "call_x", strlen("call_x"), "read_file",
                         strlen("read_file"), big, TOOL_MAX_ARGS + 1) == -1,
          "overflow: whole call past the cap rejected");
    check(ta.ncalls == 0 && !ta.used[0],
          "overflow: failed whole call claims no slot");
    check(tool_accum_add(&ta, 1, TOOL_MAX_CALLS, "call_x", strlen("call_x"),
                         "read_file", strlen("read_file"), "{}", 2) == -1,
          "overflow: out-of-range index rejected");
    check(tool_accum_add(&ta, 1, -1, "call_x", strlen("call_x"), "read_file",
                         strlen("read_file"), "{}", 2) == -1,
          "overflow: negative index rejected");

    int filled = 1;
    for (int i = 0; i < TOOL_MAX_CALLS; i++) {
        if (tool_accum_add(&ta, 0, 0, NULL, 0, "read_file",
                           strlen("read_file"), "{}", 2) != 0) {
            filled = 0;
        }
    }
    check(filled && ta.ncalls == TOOL_MAX_CALLS,
          "overflow: every slot can be filled");
    check(tool_accum_add(&ta, 0, 0, NULL, 0, "read_file",
                         strlen("read_file"), "{}", 2) == -1,
          "overflow: a ninth id-less call is rejected");
    check(ta.ncalls == TOOL_MAX_CALLS,
          "overflow: rejected ninth call leaves ncalls intact");
    tool_accum_clear(&ta);
}

int main(void) {
    test_whole();
    test_fragments();
    test_fragment_continuation();
    test_sparse();
    test_id_reuse();
    test_idless();
    test_empty_args();
    test_clear_reset();
    test_overflow();
    if (fails) {
        printf("FAILED: %d tool_accum check(s)\n", fails);
        return 1;
    }
    printf("TOOL_ACCUM OK (%d checks)\n", checks);
    return 0;
}
