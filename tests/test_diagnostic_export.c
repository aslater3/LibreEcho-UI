#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Include the production formatter so this exercises the export log path. */
#define LE_OS_VERSION "test"
#define LE_SOURCE_COMMIT "test"
#define LE_SOURCE_DIRTY "0"
#define LE_SOURCE_DIGEST "test"
#include "../src/diagnostic_export.c"

int main(void)
{
    struct api_context context;
    struct api_response response;
    struct diag_writer writer;
    char parsed_level[32];
    char wrapped[sizeof(response.body) + 3];

    memset(&context, 0, sizeof(context));
    memset(&response, 0, sizeof(response));
    snprintf(context.logs[0], sizeof(context.logs[0]),
             "{\"level\":\"x%s%sbad\",\"boot_seconds\":7,\"message\":\"ok\"}",
             "\\\"", "\\\\");
    snprintf(context.logs[1], sizeof(context.logs[1]),
             "{\"level\":\"ctrl\001bad\",\"boot_seconds\":8,\"message\":\"control\"}");
    assert(json_get_string(context.logs[0], "level", parsed_level, sizeof(parsed_level)) == 1);
    assert(parsed_level[0] == 'x' && parsed_level[1] == '"' &&
           parsed_level[2] == '\\' && !strcmp(parsed_level + 3, "bad"));
    context.log_count = 2;
    context.log_next = 2;
    writer.response = &response;
    writer.used = 0;
    writer.failed = 0;

    assert(append_logs(&writer, &context) == 0);
    assert(writer.used < sizeof(wrapped) - 2);
    assert(writer.used > 0 && response.body[writer.used - 1] == ',');
    response.body[--writer.used] = '\0';
    snprintf(wrapped, sizeof(wrapped), "{%.*s}", (int)writer.used, response.body);
    assert(json_valid_object(wrapped, strlen(wrapped)));
    puts("diagnostic export formatter: ok");
    return 0;
}
