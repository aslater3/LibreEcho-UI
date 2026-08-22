#define _POSIX_C_SOURCE 200809L

#include "adapter/llm_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct events {
    int count;
    char joined[256];
};

struct body_capture {
    size_t bytes;
    size_t stored;
    char prefix[128];
};

static int collect_body(void *context, const char *data, size_t size)
{
    struct body_capture *capture = context;
    size_t room = sizeof(capture->prefix) - 1U - capture->stored;
    size_t copy = size < room ? size : room;

    capture->bytes += size;
    if (copy) {
        memcpy(capture->prefix + capture->stored, data, copy);
        capture->stored += copy;
        capture->prefix[capture->stored] = '\0';
    }
    return 0;
}

static int collect(void *context, const char *data)
{
    struct events *events = context;

    if (strlen(events->joined) + strlen(data) + 2 >=
        sizeof(events->joined))
        return -1;
    if (events->joined[0])
        strcat(events->joined, "\n");
    strcat(events->joined, data);
    ++events->count;
    return 0;
}

int main(void)
{
    char capture[] = "/tmp/libreecho-http-capture-XXXXXX";
    char config[16384];
    struct le_llm_http_request request;
    struct le_llm_http_response response;
    struct events events;
    struct body_capture streamed;
    FILE *file;
    int fd;
    size_t count;

    fd = mkstemp(capture);
    CHECK(fd >= 0);
    close(fd);
    CHECK(setenv("LE_TEST_CURL_CAPTURE", capture, 1) == 0);
    CHECK(setenv("LE_TEST_CURL_MODE", "json", 1) == 0);
    memset(&request, 0, sizeof(request));
    strcpy(request.method, "POST");
    strcpy(request.url,
           "https://auth.openai.com/api/accounts/deviceauth/usercode");
    strcpy(request.content_type, "application/json");
    strcpy(request.body, "{\"client_id\":\"test\"}");
    CHECK(le_llm_http_execute("./build/mock-llm-curl", &request,
                              NULL, NULL, &response) == 0);
    CHECK(response.status == 200);
    CHECK(strstr(response.body, "\"user_code\":\"TEST-CODE\"") != NULL);

    memset(&request, 0, sizeof(request));
    strcpy(request.method, "POST");
    strcpy(request.url,
           "https://chatgpt.com/backend-api/codex/responses");
    strcpy(request.content_type, "application/json");
    strcpy(request.authorization, "Bearer secret-token-value");
    strcpy(request.account_id, "account-123");
    strcpy(request.body, "{\"stream\":true}");
    request.accept_sse = 1;
    memset(&events, 0, sizeof(events));
    CHECK(setenv("LE_TEST_CURL_MODE", "sse", 1) == 0);
    CHECK(le_llm_http_execute("./build/mock-llm-curl", &request,
                              collect, &events, &response) == 0);
    CHECK(response.status == 200);
    CHECK(events.count == 2);
    CHECK(strstr(events.joined, "\"delta\":\"Hello\"") != NULL);

    file = fopen(capture, "r");
    CHECK(file != NULL);
    count = fread(config, 1, sizeof(config) - 1, file);
    fclose(file);
    config[count] = '\0';
    CHECK(strstr(config,
                 "Authorization: Bearer secret-token-value") != NULL);
    CHECK(strstr(config, "ChatGPT-Account-Id: account-123") != NULL);
    CHECK(strstr(config, "proto = \"=https\"") != NULL);
    CHECK(strstr(config, "insecure") == NULL);

    memset(&request, 0, sizeof(request));
    strcpy(request.method, "POST");
    strcpy(request.url, "http://198.51.100.20:8000/v1/chat/completions");
    strcpy(request.content_type, "application/json");
    strcpy(request.body, "{\"stream\":true}");
    request.accept_sse = 1;
    request.allow_insecure_http = 1;
    CHECK(le_llm_http_execute("./build/mock-llm-curl", &request,
                              collect, &events, &response) == 0);
    file = fopen(capture, "r");
    CHECK(file != NULL);
    count = fread(config, 1, sizeof(config) - 1, file);
    fclose(file);
    config[count] = '\0';
    CHECK(strstr(config, "proto = \"=http,https\"") != NULL);
    CHECK(strstr(config, "tlsv1.2") == NULL);

    memset(&events, 0, sizeof(events));
    CHECK(setenv("LE_TEST_CURL_MODE", "buffered", 1) == 0);
    CHECK(le_llm_http_execute("./build/mock-llm-curl", &request,
                              collect, &events, &response) == 0);
    CHECK(response.status == 200);
    CHECK(events.count == 1);
    CHECK(strstr(events.joined,
                 "\"content\":\"Local ready\"") != NULL);

    memset(&request, 0, sizeof(request));
    strcpy(request.method, "GET");
    strcpy(request.url,
           "https://api.met.no/weatherapi/locationforecast/2.0/compact"
           "?lat=30.2672&lon=-97.7431");
    request.connect_timeout_seconds = 1;
    request.max_time_seconds = 1;
    memset(&streamed, 0, sizeof(streamed));
    CHECK(setenv("LE_TEST_CURL_MODE", "weather-met-no-large", 1) == 0);
    CHECK(le_llm_http_execute_stream(
              "./build/mock-llm-curl", &request, collect_body, &streamed,
              &response) == 0);
    CHECK(response.status == 200);
    CHECK(streamed.bytes > 16384);
    CHECK(strstr(streamed.prefix, "air_temperature") != NULL);
    file = fopen(capture, "r");
    CHECK(file != NULL);
    count = fread(config, 1, sizeof(config) - 1, file);
    fclose(file);
    config[count] = '\0';
    CHECK(strstr(config, "connect-timeout = 1") != NULL);
    CHECK(strstr(config, "max-time = 1") != NULL);

    unlink(capture);
    puts("llm http: private pipe config, TLS, SSE and JSON fallback: ok");
    return 0;
}
