#pragma once
/* JSON output (docs/design/json.md): a string escaper and a streaming
 * writer. The writer places the commas and checks the nesting, so a
 * program writes values, keys, objects and arrays in document order. */
#include <stdbool.h>
#include <stdio.h>

#define JSON_MAX_DEPTH 32

/* Write s as a JSON string with its quotes. '"', '\\', control characters
 * and DEL are escaped as \uXXXX or \" and \\. Valid UTF-8 sequences are
 * copied unchanged. A byte that does not begin a valid sequence is written
 * as \u00XX, the code point of the byte in Latin-1. NULL is written as an
 * empty string. */
void json_write_string(FILE *f, const char *s);

/* A writer. depth counts the open objects and arrays. count[i] is the
 * number of members already written at depth i. after_key is set between a
 * key and its value. error is set by a call out of order, such as a key
 * outside an object or a value without a key in an object; such a call
 * writes nothing. */
struct json_writer {
    FILE *f;
    int depth;
    unsigned count[JSON_MAX_DEPTH + 1];
    bool in_object[JSON_MAX_DEPTH + 1];
    bool after_key;
    bool error;
};

void json_init(struct json_writer *w, FILE *f);
void json_begin_object(struct json_writer *w);
void json_end_object(struct json_writer *w);
void json_begin_array(struct json_writer *w);
void json_end_array(struct json_writer *w);
/* The key of the next member of the current object. */
void json_key(struct json_writer *w, const char *key);
void json_string(struct json_writer *w, const char *s);
void json_int(struct json_writer *w, long long v);
void json_uint(struct json_writer *w, unsigned long long v);
void json_bool(struct json_writer *w, bool v);
void json_null(struct json_writer *w);
/* Finish the document with a newline. Returns 0, or -1 when a call was
 * out of order, a container is still open, or the stream reported an
 * error. */
int json_finish(struct json_writer *w);
