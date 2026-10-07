/* json_check.h - a minimal recursive-descent JSON validator for the tests.
 *
 * The API writer builds JSON by hand, so "it looked right in the browser" is
 * not evidence. This parses the whole document strictly and reports the offset
 * of the first problem, which turns a malformed response into a precise test
 * failure.
 */
#ifndef AERIS_JSON_CHECK_H
#define AERIS_JSON_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char *jc_value(const char *p);

static const char *jc_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *jc_string(const char *p)
{
    if (*p != '"') return NULL;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (!*p) return NULL;
            if (*p == 'u') {
                for (int i = 0; i < 4; i++) {
                    p++;
                    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
                          (*p >= 'A' && *p <= 'F'))) return NULL;
                }
            }
            p++;
        } else if ((unsigned char)*p < 0x20) {
            return NULL;            /* raw control character: not legal JSON */
        } else p++;
    }
    return (*p == '"') ? p + 1 : NULL;
}

static const char *jc_number(const char *p)
{
    const char *start = p;
    if (*p == '-') p++;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') { while (*p >= '0' && *p <= '9') p++; }
    else return NULL;
    if (*p == '.') {
        p++;
        if (!(*p >= '0' && *p <= '9')) return NULL;
        while (*p >= '0' && *p <= '9') p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!(*p >= '0' && *p <= '9')) return NULL;
        while (*p >= '0' && *p <= '9') p++;
    }
    return (p > start) ? p : NULL;
}

static const char *jc_array(const char *p)
{
    p = jc_ws(p + 1);
    if (*p == ']') return p + 1;
    for (;;) {
        p = jc_value(jc_ws(p));
        if (!p) return NULL;
        p = jc_ws(p);
        if (*p == ',') { p++; continue; }
        return (*p == ']') ? p + 1 : NULL;
    }
}

static const char *jc_object(const char *p)
{
    p = jc_ws(p + 1);
    if (*p == '}') return p + 1;
    for (;;) {
        p = jc_string(jc_ws(p));
        if (!p) return NULL;
        p = jc_ws(p);
        if (*p != ':') return NULL;
        p = jc_value(jc_ws(p + 1));
        if (!p) return NULL;
        p = jc_ws(p);
        if (*p == ',') { p++; continue; }
        return (*p == '}') ? p + 1 : NULL;
    }
}

static const char *jc_value(const char *p)
{
    switch (*p) {
    case '{': return jc_object(p);
    case '[': return jc_array(p);
    case '"': return jc_string(p);
    case 't': return strncmp(p, "true", 4)  ? NULL : p + 4;
    case 'f': return strncmp(p, "false", 5) ? NULL : p + 5;
    case 'n': return strncmp(p, "null", 4)  ? NULL : p + 4;
    default:  return jc_number(p);
    }
}

/* Returns -1 when the document is valid, otherwise the byte offset where
 * parsing stopped. */
static inline long json_check(const char *doc)
{
    const char *end = jc_value(jc_ws(doc));
    if (!end) return 0;
    end = jc_ws(end);
    if (*end != '\0') return (long)(end - doc);
    return -1;
}

#endif /* AERIS_JSON_CHECK_H */
