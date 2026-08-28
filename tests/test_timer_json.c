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

    cursor = le_test_next_array_object(cursor, object, sizeof(object));
    assert(cursor != NULL);
    assert(strstr(object, "\"id\":1") != NULL);
    assert(strstr(object, "curly } { and \\\"quote\\\"") != NULL);
    cursor = le_test_next_array_object(cursor, object, sizeof(object));
    assert(cursor != NULL);
    assert(strstr(object, "\"id\":2") != NULL);
    assert(strstr(object, "second") != NULL);
    puts("timer JSON object scanner: ok");
    return 0;
}
