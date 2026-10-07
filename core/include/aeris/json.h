/* json.h - a bounded append-only JSON writer.
 *
 * No cJSON, no heap. Every endpoint writes into a caller-owned buffer and the
 * writer latches an overflow flag instead of truncating silently, so an
 * endpoint that outgrows its buffer fails loudly in tests rather than emitting
 * invalid JSON to the browser.
 */
#ifndef AERIS_JSON_H
#define AERIS_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
    bool    overflow;
} aeris_json_t;

void aeris_json_init(aeris_json_t *j, char *buf, size_t cap);
void aeris_json_raw(aeris_json_t *j, const char *s);
void aeris_json_obj_open(aeris_json_t *j);
void aeris_json_obj_close(aeris_json_t *j);
void aeris_json_arr_open(aeris_json_t *j);
void aeris_json_arr_close(aeris_json_t *j);
void aeris_json_comma(aeris_json_t *j);
void aeris_json_key(aeris_json_t *j, const char *key);
void aeris_json_str(aeris_json_t *j, const char *key, const char *val);
void aeris_json_i32(aeris_json_t *j, const char *key, int32_t val);
void aeris_json_u32(aeris_json_t *j, const char *key, uint32_t val);
void aeris_json_f(aeris_json_t *j, const char *key, float val, int decimals);
void aeris_json_bool(aeris_json_t *j, const char *key, bool val);
bool aeris_json_ok(const aeris_json_t *j);

#endif /* AERIS_JSON_H */
