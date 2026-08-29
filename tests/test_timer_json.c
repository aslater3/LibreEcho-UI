#include "json.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

const char *le_test_next_array_object(const char *, char *, size_t);

int main(void)
{
    const char *json = "{\"timers\":[{\"id\":1,\"label\":\"curly } { and \\\"quote\\\"\"},{\"id\":2,\"label\":\"second\"}]}";
    const char *cursor = strstr(json, "\"timers\"");
    char object[256];
    long long seconds = 0;
    char label[48];

    cursor = le_test_next_array_object(cursor, object, sizeof(object));
    assert(cursor != NULL);
    assert(strstr(object, "\"id\":1") != NULL);
    assert(strstr(object, "curly } { and \\\"quote\\\"") != NULL);
    cursor = le_test_next_array_object(cursor, object, sizeof(object));
    assert(cursor != NULL);
    assert(strstr(object, "\"id\":2") != NULL);
    assert(strstr(object, "second") != NULL);

    assert(json_get_int64("{\"seconds\":4294967297}", "seconds",
                          &seconds) == 1);
    assert(seconds == 4294967297LL);
    assert(json_get_int64("{\"seconds\":1.5}", "seconds", &seconds) < 1);
    assert(json_get_int64_top_level("{\"meta\":{\"seconds\":1},\"seconds\":999999}",
                                    "seconds", &seconds) == 1);
    assert(seconds == 999999);
    assert(json_get_string_top_level("{\"meta\":{\"label\":\"wrong\"},\"label\":\"caf\\u00e9\"}",
                                     "label", label, sizeof(label)) == 1);
    assert(!strcmp(label, "caf\xc3\xa9"));
    assert(json_get_string("{\"label\":\"\\ud83d\\ude00\"}", "label",
                           label, sizeof(label)) == 1);
    assert(!strcmp(label, "\xf0\x9f\x98\x80"));
    puts("timer JSON object scanner, nesting, Unicode, and integer range: ok");
    return 0;
}
