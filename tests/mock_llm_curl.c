#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *capture = getenv("LE_TEST_CURL_CAPTURE");
    const char *mode = getenv("LE_TEST_CURL_MODE");
    char buffer[16384];
    char body[16384];
    size_t config_used = 0;
    size_t body_used = 0;
    FILE *input;
    FILE *output;
    int response_route;
    int compatible_route;
    int poll_route;
    int token_route;
    int i;

    for (i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--config"))
            config_path = argv[i + 1];
    if (!config_path || !capture)
        return 2;
    input = fopen(config_path, "r");
    output = fopen(capture, "w");
    if (!input || !output)
        return 3;
    while (config_used + 1 < sizeof(buffer)) {
        int c = fgetc(input);

        if (c == EOF)
            break;
        buffer[config_used++] = (char)c;
        fputc(c, output);
    }
    buffer[config_used] = '\0';
    fclose(input);
    fclose(output);
    response_route =
        strstr(buffer, "/backend-api/codex/responses") != NULL;
    compatible_route =
        strstr(buffer, "/v1/chat/completions") != NULL;
    poll_route =
        strstr(buffer, "/api/accounts/deviceauth/token") != NULL;
    token_route = strstr(buffer, "/oauth/token") != NULL;
    while (body_used + 1 < sizeof(body)) {
        size_t count = fread(
            body + body_used, 1, sizeof(body) - body_used - 1, stdin);

        if (!count)
            break;
        body_used += count;
    }
    body[body_used] = '\0';
    if ((mode && !strcmp(mode, "sse")) ||
        (!mode && response_route)) {
        const char *reply = strstr(body, "Previous assistant:")
            ? "Thanks." : strstr(body, "mock transcription")
                ? "What else?" : "Hello";

        fputs("event: response.output_text.delta\n", stdout);
        fprintf(stdout,
                "data: {\"type\":\"response.output_text.delta\","
                "\"delta\":\"%s\"}\n\n", reply);
        fputs("data: {\"type\":\"response.completed\"}\n\n", stdout);
    } else if ((mode && !strcmp(mode, "buffered")) ||
               (!mode && compatible_route)) {
        fputs("{\"choices\":[{\"finish_reason\":\"stop\","
              "\"index\":0,\"message\":{\"role\":\"assistant\","
              "\"content\":\"Local ready\"}}]}\n", stdout);
    } else if (!mode && poll_route) {
        fputs("{\"authorization_code\":\"test-auth-code\","
              "\"code_verifier\":\"test-code-verifier\"}\n", stdout);
    } else if (!mode && token_route) {
        fputs("{\"access_token\":\"header."
              "eyJleHAiOjE5MDAwMDAwMDAsImh0dHBzOi8vYXBpLm9wZW5haS5jb20v"
              "YXV0aCI6eyJjaGF0Z3B0X2FjY291bnRfaWQiOiJhY2N0LTEyMyJ9fQ."
              "signature\",\"refresh_token\":\"test-refresh-token\"}\n",
              stdout);
    } else {
        fputs("{\"user_code\":\"TEST-CODE\","
              "\"device_auth_id\":\"test-device\",\"interval\":3}\n",
              stdout);
    }
    fputs("\n__LE_HTTP_STATUS__:200\n", stdout);
    return 0;
}
