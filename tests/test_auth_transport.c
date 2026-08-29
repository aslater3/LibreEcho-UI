#include "api.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    char users[128], sessions[160], bootstrap_users[128];
    char bootstrap_sessions[160], csrf[65], token[LE_AUTH_TOKEN_MAX];
    char http_token[LE_AUTH_TOKEN_MAX];
    char evicting_http_token[LE_AUTH_TOKEN_MAX];
    char filled_tokens[LE_AUTH_MAX_SESSIONS][LE_AUTH_TOKEN_MAX];
    struct le_auth_db seed;
    struct le_backend *backend = NULL;
    struct api_context api;
    struct api_context bootstrap;
    struct api_request request;
    struct api_response response;
    const char body[] = "{\"username\":\"test-user\",\"password\":\"test-password-123\"}";
    int i;

    snprintf(users, sizeof(users), "/tmp/libreecho-auth-transport-users-%ld", (long)getpid());
    snprintf(sessions, sizeof(sessions), "%s.sessions", users);
    snprintf(bootstrap_users, sizeof(bootstrap_users),
             "/tmp/libreecho-auth-transport-bootstrap-%ld", (long)getpid());
    snprintf(bootstrap_sessions, sizeof(bootstrap_sessions), "%s.sessions",
             bootstrap_users);
    unlink(users);
    unlink(sessions);
    unlink(bootstrap_users);
    unlink(bootstrap_sessions);
    memset(&seed, 0, sizeof(seed));
    CHECK(le_auth_add_user(&seed, users, "test-user", "test-password-123") == 0);
    CHECK(le_backend_init(&backend, "mock", "config/mock-state.json", NULL, 42) == LE_OK);
    for (i = 0; i < 64; ++i) csrf[i] = 'a';
    csrf[64] = '\0';
    CHECK(api_init(&api, backend, 0, 0, NULL, NULL, csrf, NULL, users) == 0);
    api_set_https_active(&api, 1);

    memset(&request, 0, sizeof(request));
    snprintf(request.method, sizeof(request.method), "POST");
    snprintf(request.path, sizeof(request.path), "/api/v1/auth/login");
    snprintf(request.host, sizeof(request.host), "127.0.0.1");
    snprintf(request.origin, sizeof(request.origin), "http://127.0.0.1");
    snprintf(request.csrf, sizeof(request.csrf), "%s", csrf);
    request.body = body;
    request.body_len = strlen(body);
    request.https = 0;
    memset(&response, 0, sizeof(response));
    api_handle(&api, &request, &response);
    CHECK(response.status == 200);
    {
        const char *start = strstr(response.body, "\"token\":\"");
        CHECK(start != NULL);
        start += strlen("\"token\":\"");
        CHECK(sscanf(start, "%64[0-9a-f]", http_token) == 1);
    }
    CHECK(access(sessions, F_OK) != 0);

    request.https = 1;
    memset(&response, 0, sizeof(response));
    api_handle(&api, &request, &response);
    CHECK(response.status == 200);
    CHECK(access(sessions, F_OK) == 0);

    {
        const char *start = strstr(response.body, "\"token\":\"");
        CHECK(start != NULL);
        start += strlen("\"token\":\"");
        CHECK(sscanf(start, "%64[0-9a-f]", token) == 1);
    {
        char persisted[256] = {0};
        FILE *file = fopen(sessions, "r");
        CHECK(file != NULL);
        CHECK(fread(persisted, 1, sizeof(persisted) - 1, file) < sizeof(persisted));
        CHECK(fclose(file) == 0);
        CHECK(strstr(persisted, token) != NULL);
        CHECK(strstr(persisted, http_token) == NULL);
    }
    }
    snprintf(request.method, sizeof(request.method), "POST");
    snprintf(request.path, sizeof(request.path), "/api/v1/auth/logout");
    snprintf(request.authorization, sizeof(request.authorization),
             "Bearer %s", token);
    request.body = "{}";
    request.body_len = 2;
    request.https = 0; /* HTTP logout still invalidates the HTTPS-persisted row. */
    memset(&response, 0, sizeof(response));
    api_handle(&api, &request, &response);
    CHECK(response.status == 200);
    {
        char persisted[256] = {0};
        FILE *file = fopen(sessions, "r");
        CHECK(file != NULL);
        CHECK(fread(persisted, 1, sizeof(persisted) - 1, file) < sizeof(persisted));
        CHECK(fclose(file) == 0);
        CHECK(strstr(persisted, token) == NULL);
        CHECK(strstr(persisted, http_token) == NULL);
    }

    snprintf(request.path, sizeof(request.path), "/api/v1/auth/logout");
    snprintf(request.authorization, sizeof(request.authorization),
             "Bearer %s", http_token);
    request.https = 0;
    memset(&response, 0, sizeof(response));
    api_handle(&api, &request, &response);
    CHECK(response.status == 200);

    /* An HTTP login may evict a persisted HTTPS session. The persisted-only
       rewrite must remove the evicted token without storing the HTTP token. */
    for (i = 0; i < LE_AUTH_MAX_SESSIONS; ++i) {
        const char *start;
        request.https = 1;
        snprintf(request.path, sizeof(request.path), "/api/v1/auth/login");
        request.authorization[0] = '\0';
        request.body = body;
        request.body_len = strlen(body);
        memset(&response, 0, sizeof(response));
        api_handle(&api, &request, &response);
        CHECK(response.status == 200);
        start = strstr(response.body, "\"token\":\"");
        CHECK(start != NULL);
        start += strlen("\"token\":\"");
        CHECK(sscanf(start, "%64[0-9a-f]", filled_tokens[i]) == 1);
    }
    request.https = 0;
    memset(&response, 0, sizeof(response));
    api_handle(&api, &request, &response);
    CHECK(response.status == 200);
    {
        const char *start = strstr(response.body, "\"token\":\"");
        CHECK(start != NULL);
        start += strlen("\"token\":\"");
        CHECK(sscanf(start, "%64[0-9a-f]", evicting_http_token) == 1);
    }
    {
        char persisted[1024] = {0};
        FILE *file = fopen(sessions, "r");
        int lines = 0;
        char *line;
        CHECK(file != NULL);
        CHECK(fread(persisted, 1, sizeof(persisted) - 1, file) < sizeof(persisted));
        CHECK(fclose(file) == 0);
        for (line = persisted; *line; ++line)
            if (*line == '\n')
                ++lines;
        CHECK(lines == LE_AUTH_MAX_SESSIONS - 1);
        CHECK(strstr(persisted, evicting_http_token) == NULL);
        CHECK(strstr(persisted, filled_tokens[0]) == NULL);
        CHECK(strstr(persisted, filled_tokens[LE_AUTH_MAX_SESSIONS - 1]) != NULL);
    }

    CHECK(api_init(&bootstrap, backend, 0, 0, NULL, NULL, csrf, NULL,
                   bootstrap_users) == 0);
    api_set_https_active(&bootstrap, 1);
    snprintf(request.path, sizeof(request.path), "/api/v1/auth/bootstrap");
    request.authorization[0] = '\0';
    request.body = "{\"username\":\"bootstrap\",\"password\":\"bootstrap-password\",\"password_confirm\":\"bootstrap-password\"}";
    request.body_len = strlen(request.body);
    request.https = 1;
    memset(&response, 0, sizeof(response));
    api_handle(&bootstrap, &request, &response);
    CHECK(response.status == 200);
    CHECK(access(bootstrap_sessions, F_OK) == 0);

    unlink(users);
    unlink(sessions);
    unlink(bootstrap_users);
    unlink(bootstrap_sessions);
    le_backend_destroy(backend);
    puts("auth session transport persistence: ok");
    return 0;
}
