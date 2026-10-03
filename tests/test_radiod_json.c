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
    char response[512];
    const char request_with_url[] =
        "{\"url\":\"/music/one\\\"two\\\\backslash.mp3\"}";

    CHECK(json_string_field(request_with_url, "url", url, sizeof(url)) == 0);
    CHECK(!strcmp(url, "/music/one\"two\\backslash.mp3"));

    /*
     * The status document must publish the Opus capability so the HTTP layer
     * can gate an Opus launch before it happens.  Whatever this build's value
     * is, the field has to be present and boolean.
     */
    build_status_json(response, sizeof(response));
    CHECK(strstr(response, "\"opus\":true") != NULL ||
          strstr(response, "\"opus\":false") != NULL);

    puts("radiod escaped URL parsing and status Opus capability: ok");
    return 0;
}
