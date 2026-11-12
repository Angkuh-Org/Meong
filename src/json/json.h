#ifndef MEONG_JSON_H
#define MEONG_JSON_H

#include "util/util.h"

enum {
    /* jsmn token types are bit flags; json.c asserts they still match. */
    J_UNDEFINED = 0,
    J_OBJECT = 1,
    J_ARRAY = 2,
    J_STRING = 4,
    J_PRIMITIVE = 8,
};

typedef struct {
    void *tok;
    int *skip;
    int ntok;
    int cap;
    const char *text;
} JDoc;

int jdoc_parse(JDoc *d, const char *text, size_t len);
void jdoc_free(JDoc *d);

int jdoc_root(const JDoc *d);
int jget(const JDoc *d, int obj, const char *key);
int jlen(const JDoc *d, int arr);
int jgeti(const JDoc *d, int arr, int index);
int jtype(const JDoc *d, int tok);
int jstring(const JDoc *d, int tok, Buf *out);
char *jstring_alloc(const JDoc *d, int tok);
long long jint(const JDoc *d, int tok, long long def);

#endif
