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
    puts("timer JSON object scanner and integer range: ok");
    return 0;
}
