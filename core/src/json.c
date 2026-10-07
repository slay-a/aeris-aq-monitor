#include <math.h>
#include <stdio.h>
#include <string.h>
#include "aeris/json.h"

void aeris_json_init(aeris_json_t *j, char *buf, size_t cap)
{
    j->buf = buf; j->cap = cap; j->len = 0; j->overflow = false;
    if (cap) buf[0] = '\0';
}

static void put(aeris_json_t *j, const char *s, size_t n)
{
    if (j->overflow) return;
    if (j->len + n + 1 > j->cap) { j->overflow = true; return; }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = '\0';
}

void aeris_json_raw(aeris_json_t *j, const char *s) { put(j, s, strlen(s)); }
void aeris_json_obj_open(aeris_json_t *j)  { put(j, "{", 1); }
void aeris_json_obj_close(aeris_json_t *j) { put(j, "}", 1); }
void aeris_json_arr_open(aeris_json_t *j)  { put(j, "[", 1); }
void aeris_json_arr_close(aeris_json_t *j) { put(j, "]", 1); }
void aeris_json_comma(aeris_json_t *j)     { put(j, ",", 1); }

/* Escapes only what our own key and value strings can contain: quote,
 * backslash and control characters. No user-supplied text reaches this
 * writer, but escaping anyway keeps a future label string from breaking
 * the response. */
static void put_quoted(aeris_json_t *j, const char *s)
{
    put(j, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"')       put(j, "\\\"", 2);
        else if (c == '\\') put(j, "\\\\", 2);
        else if (c == '\n') put(j, "\\n", 2);
        else if (c == '\r') put(j, "\\r", 2);
        else if (c == '\t') put(j, "\\t", 2);
        else if (c < 0x20) {
            char esc[7];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            put(j, esc, strlen(esc));
        } else put(j, (const char *)&c, 1);
    }
    put(j, "\"", 1);
}

void aeris_json_key(aeris_json_t *j, const char *key)
{
    put_quoted(j, key);
    put(j, ":", 1);
}

void aeris_json_str(aeris_json_t *j, const char *key, const char *val)
{
    aeris_json_key(j, key);
    put_quoted(j, val ? val : "");
}

void aeris_json_i32(aeris_json_t *j, const char *key, int32_t val)
{
    aeris_json_key(j, key);
    char tmp[12];
    snprintf(tmp, sizeof tmp, "%ld", (long)val);
    put(j, tmp, strlen(tmp));
}

void aeris_json_u32(aeris_json_t *j, const char *key, uint32_t val)
{
    aeris_json_key(j, key);
    char tmp[12];
    snprintf(tmp, sizeof tmp, "%lu", (unsigned long)val);
    put(j, tmp, strlen(tmp));
}

void aeris_json_f(aeris_json_t *j, const char *key, float val, int decimals)
{
    aeris_json_key(j, key);
    /* JSON has no NaN literal; null is the honest answer for a sensor that
     * has not reported yet, and the dashboard already handles it. */
    if (!(val == val) || isinf(val)) { put(j, "null", 4); return; }
    char tmp[32];
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    snprintf(tmp, sizeof tmp, "%.*f", decimals, (double)val);
    put(j, tmp, strlen(tmp));
}

void aeris_json_bool(aeris_json_t *j, const char *key, bool val)
{
    aeris_json_key(j, key);
    if (val) put(j, "true", 4); else put(j, "false", 5);
}

bool aeris_json_ok(const aeris_json_t *j) { return !j->overflow; }
