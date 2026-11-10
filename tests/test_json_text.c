/* json_get_text(): standard JSON escapes decode for multi-line text (PEM),
 * while json_get_string() keeps rejecting every control character. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int text(const char *doc, char *o, size_t z) { return json_get_text(doc, "v", o, z); }
int main(void)
{
    char o[128];
    /* every standard escape that is legal in text */
    CHECK(text("{\"v\":\"a\\nb\\r\\nc\\td\\\"e\\\\f\\/g\"}", o, sizeof o) == 1);
    CHECK(!strcmp(o, "a\nb\r\nc\td\"e\\f/g"));
    CHECK(text("{\"v\":\"x\\u000ay\\u0009z\\u000dw\"}", o, sizeof o) == 1 && !strcmp(o, "x\ny\tz\rw"));
    CHECK(text("{\"v\":\"\\u00e9\\u20ac\\ud83d\\ude00\"}", o, sizeof o) == 1 &&
          !strcmp(o, "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"));
    CHECK(text("{\"v\":\"\"}", o, sizeof o) == 1 && o[0] == 0);
    /* rejected: NUL, other control escapes, bad escapes, raw control bytes, lone surrogates */
    CHECK(text("{\"v\":\"a\\u0000b\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"a\\u0001b\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"a\\u001fb\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"a\\bb\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"a\\fb\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"a\\xb\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"a\\u12\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"a\\u12zzb\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"a\\ud800b\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"a\\udc00b\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"a\nb\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"a\tb\"}", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"abc", o, sizeof o) == -1);
    CHECK(text("{\"v\":\"abc\\", o, sizeof o) == -1);
    CHECK(text("{\"w\":\"abc\"}", o, sizeof o) == 0);
    /* json_get_string() is unchanged: no control character gets through */
    CHECK(json_get_string("{\"v\":\"a\\nb\"}", "v", o, sizeof o) == -1);
    CHECK(json_get_string("{\"v\":\"a\\tb\"}", "v", o, sizeof o) == -1);
    CHECK(json_get_string("{\"v\":\"a\\rb\"}", "v", o, sizeof o) == -1);
    CHECK(json_get_string("{\"v\":\"a\\u000ab\"}", "v", o, sizeof o) == -1);
    CHECK(json_get_string("{\"v\":\"bad\\u0001\"}", "v", o, sizeof o) == -1);
    CHECK(json_get_string("{\"v\":\"a\\\"b\\\\c\\u00e9\"}", "v", o, sizeof o) == 1 && !strcmp(o, "a\"b\\c\xc3\xa9"));
    /* capacity: exact fit passes, one byte over fails, never overruns */
    CHECK(text("{\"v\":\"a\\nb\"}", o, 4) == 1 && !strcmp(o, "a\nb"));
    CHECK(text("{\"v\":\"a\\nb\"}", o, 3) == -1);
    CHECK(text("{\"v\":\"a\"}", o, 0) == -1);
    /* very long, escape-heavy string (a multi-certificate PEM bundle size) */
    {
        size_t lines = 600, i, n = 0, cap = lines * 65 + 1;
        char *doc = malloc(lines * 66 + 32), *big = malloc(cap + 1), *p;
        p = doc + sprintf(doc, "{\"v\":\"");
        for (i = 0; i < lines; i++) {
            memset(p, 'A', 64); p += 64; *p++ = '\\'; *p++ = 'n';
        }
        strcpy(p, "\"}");
        CHECK(json_get_text(doc, "v", big, cap) == 1);
        n = strlen(big);
        CHECK(n == lines * 65 && big[64] == '\n' && big[n - 1] == '\n');
        CHECK(json_get_text(doc, "v", big, cap - 1) == -1);
        free(doc); free(big);
    }
    if (fails) return 1;
    puts("json text: ok");
    return 0;
}
