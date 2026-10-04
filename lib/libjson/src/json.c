/* JSON output (json/json.h, docs/design/json.md). */
#include <json/json.h>

/* The length of the valid UTF-8 sequence at p, or 0 when p does not begin
 * one (RFC 3629: no overlong forms, no surrogates, at most U+10FFFF). */
static int utf8_length(const unsigned char *p)
{
    unsigned c = p[0];
    if (c >= 0xc2 && c <= 0xdf)
        return (p[1] & 0xc0) == 0x80 ? 2 : 0;
    if (c >= 0xe0 && c <= 0xef) {
        unsigned lo = c == 0xe0 ? 0xa0 : 0x80, hi = c == 0xed ? 0x9f : 0xbf;
        return p[1] >= lo && p[1] <= hi && (p[2] & 0xc0) == 0x80 ? 3 : 0;
    }
    if (c >= 0xf0 && c <= 0xf4) {
        unsigned lo = c == 0xf0 ? 0x90 : 0x80, hi = c == 0xf4 ? 0x8f : 0xbf;
        return p[1] >= lo && p[1] <= hi && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80 ? 4 : 0;
    }
    return 0;
}

void json_write_string(FILE *f, const char *s)
{
    fputc('"', f);
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
            fputc(*p++, f);
        } else if (*p < 0x20 || *p == 0x7f) {
            fprintf(f, "\\u%04x", *p++);
        } else if (*p < 0x80) {
            fputc(*p++, f);
        } else {
            int n = utf8_length(p);
            if (n) {
                fwrite(p, 1, (size_t)n, f);
                p += n;
            } else {
                fprintf(f, "\\u%04x", *p++);
            }
        }
    }
    fputc('"', f);
}

void json_init(struct json_writer *w, FILE *f)
{
    *w = (struct json_writer){ .f = f };
}

/* Write the separator before a value or a key, or record an error when
 * the value is out of place. Returns false after an error. */
static bool before_value(struct json_writer *w)
{
    if (w->error)
        return false;
    if (w->in_object[w->depth] && !w->after_key) {
        w->error = true;
        return false;
    }
    if (!w->in_object[w->depth] && w->depth > 0 && w->count[w->depth]++)
        fputc(',', w->f);
    if (w->depth == 0 && w->count[0]++) {
        w->error = true;        /* a second top level value */
        return false;
    }
    w->after_key = false;
    return true;
}

static void open_container(struct json_writer *w, char c, bool object)
{
    if (!before_value(w))
        return;
    if (w->depth == JSON_MAX_DEPTH) {
        w->error = true;
        return;
    }
    fputc(c, w->f);
    w->depth++;
    w->count[w->depth] = 0;
    w->in_object[w->depth] = object;
}

static void close_container(struct json_writer *w, char c, bool object)
{
    if (w->error)
        return;
    if (w->depth == 0 || w->in_object[w->depth] != object || w->after_key) {
        w->error = true;
        return;
    }
    fputc(c, w->f);
    w->depth--;
}

void json_begin_object(struct json_writer *w)
{
    open_container(w, '{', true);
}

void json_end_object(struct json_writer *w)
{
    close_container(w, '}', true);
}

void json_begin_array(struct json_writer *w)
{
    open_container(w, '[', false);
}

void json_end_array(struct json_writer *w)
{
    close_container(w, ']', false);
}

void json_key(struct json_writer *w, const char *key)
{
    if (w->error)
        return;
    if (!w->in_object[w->depth] || w->after_key) {
        w->error = true;
        return;
    }
    if (w->count[w->depth]++)
        fputc(',', w->f);
    json_write_string(w->f, key);
    fputc(':', w->f);
    w->after_key = true;
}

void json_string(struct json_writer *w, const char *s)
{
    if (before_value(w))
        json_write_string(w->f, s);
}

void json_int(struct json_writer *w, long long v)
{
    if (before_value(w))
        fprintf(w->f, "%lld", v);
}

void json_uint(struct json_writer *w, unsigned long long v)
{
    if (before_value(w))
        fprintf(w->f, "%llu", v);
}

void json_bool(struct json_writer *w, bool v)
{
    if (before_value(w))
        fputs(v ? "true" : "false", w->f);
}

void json_null(struct json_writer *w)
{
    if (before_value(w))
        fputs("null", w->f);
}

int json_finish(struct json_writer *w)
{
    if (!w->error && w->depth == 0)
        fputc('\n', w->f);
    return w->error || w->depth != 0 || ferror(w->f) ? -1 : 0;
}
