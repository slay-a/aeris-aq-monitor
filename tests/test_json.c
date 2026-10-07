#include <math.h>
#include <string.h>
#include "aeris/json.h"
#include "test.h"

void suite_json(void)
{
    SUITE("json");

    char buf[256];
    aeris_json_t j;

    aeris_json_init(&j, buf, sizeof buf);
    aeris_json_obj_open(&j);
    aeris_json_str(&j, "a", "x");       aeris_json_comma(&j);
    aeris_json_i32(&j, "b", -42);       aeris_json_comma(&j);
    aeris_json_u32(&j, "c", 7u);        aeris_json_comma(&j);
    aeris_json_f(&j, "d", 1.5f, 2);     aeris_json_comma(&j);
    aeris_json_bool(&j, "e", true);
    aeris_json_obj_close(&j);
    CHECK(aeris_json_ok(&j));
    CHECK_STR(buf, "{\"a\":\"x\",\"b\":-42,\"c\":7,\"d\":1.50,\"e\":true}");

    /* NaN has no JSON literal. null is the honest answer for a sensor that has
     * not reported, and the dashboard renders it as an em dash. */
    aeris_json_init(&j, buf, sizeof buf);
    aeris_json_obj_open(&j);
    aeris_json_f(&j, "n", nanf(""), 2); aeris_json_comma(&j);
    aeris_json_f(&j, "i", INFINITY, 2);
    aeris_json_obj_close(&j);
    CHECK(aeris_json_ok(&j));
    CHECK_STR(buf, "{\"n\":null,\"i\":null}");

    /* Escaping. */
    aeris_json_init(&j, buf, sizeof buf);
    aeris_json_str(&j, "k", "a\"b\\c\nd\te");
    CHECK(aeris_json_ok(&j));
    CHECK_STR(buf, "\"k\":\"a\\\"b\\\\c\\nd\\te\"");

    /* Overflow must latch and be reported, never truncate into invalid JSON
     * that a browser will choke on. */
    char tiny[12];
    aeris_json_init(&j, tiny, sizeof tiny);
    aeris_json_obj_open(&j);
    aeris_json_str(&j, "averylongkey", "averylongvalue");
    aeris_json_obj_close(&j);
    CHECK(!aeris_json_ok(&j));

    /* Once overflowed, further writes must be no-ops rather than corrupting
     * memory past the buffer. */
    size_t len_at_overflow = j.len;
    aeris_json_str(&j, "more", "stuff");
    CHECK_EQ_I(j.len, len_at_overflow);
    CHECK(j.len < sizeof tiny);
    CHECK_EQ_I(tiny[j.len], '\0');

    /* A zero-capacity buffer must not be written to at all. */
    aeris_json_init(&j, buf, 0);
    aeris_json_obj_open(&j);
    CHECK(!aeris_json_ok(&j));
    CHECK_EQ_I(j.len, 0);

    /* Exact-fit boundary: a payload that needs precisely cap-1 bytes plus the
     * terminator must succeed, and one byte more must fail. */
    char fit[6];
    aeris_json_init(&j, fit, sizeof fit);
    aeris_json_raw(&j, "12345");
    CHECK(aeris_json_ok(&j));
    CHECK_EQ_I(j.len, 5);
    aeris_json_raw(&j, "6");
    CHECK(!aeris_json_ok(&j));

    /* Decimal clamping: a caller asking for 99 places must not blow the
     * snprintf buffer. */
    aeris_json_init(&j, buf, sizeof buf);
    aeris_json_f(&j, "x", 3.14159265f, 99);
    CHECK(aeris_json_ok(&j));
    CHECK(strlen(buf) < 24);

    aeris_json_init(&j, buf, sizeof buf);
    aeris_json_f(&j, "x", 3.14159265f, -5);
    CHECK(aeris_json_ok(&j));
    CHECK_STR(buf, "\"x\":3");
}
