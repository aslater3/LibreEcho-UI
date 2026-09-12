#include <stdio.h>
#include <string.h>

#define main radiod_program_main
#include "../src/adapter/radiod.c"
#undef main

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    char url[256];
    const char request[] =
        "{\"url\":\"/music/one\\\"two\\\\backslash.mp3\"}";

    CHECK(json_string_field(request, "url", url, sizeof(url)) == 0);
    CHECK(!strcmp(url, "/music/one\"two\\backslash.mp3"));
    puts("radiod escaped URL parsing: ok");
    return 0;
}
