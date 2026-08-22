#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *capture = getenv("LE_TEST_CURL_CAPTURE");
    const char *mode_env = getenv("LE_TEST_CURL_MODE");
    const char *mode = mode_env;
    const char *weather_mode_file = getenv("LE_TEST_CURL_WEATHER_MODE_FILE");
    const char *weather_count = getenv("LE_TEST_CURL_WEATHER_COUNT");
    const char *body_capture = getenv("LE_TEST_CURL_BODY_CAPTURE");
    char dynamic_mode[64] = "";
    char buffer[16384];
    char body[16384];
    size_t config_used = 0;
    size_t body_used = 0;
    FILE *input;
    FILE *output;
    int response_route;
    int compatible_route;
    int weather_open_route;
    int weather_met_route;
    int weather_failure;
    int poll_route;
    int token_route;
    int status = 200;
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
    if (weather_mode_file) {
        FILE *mode_input = fopen(weather_mode_file, "r");

        if (mode_input) {
            if (fgets(dynamic_mode, sizeof(dynamic_mode), mode_input))
                dynamic_mode[strcspn(dynamic_mode, "\r\n")] = '\0';
            fclose(mode_input);
            mode = dynamic_mode;
        }
    }
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
    weather_open_route = strstr(buffer, "api.open-meteo.com") != NULL;
    weather_met_route = strstr(buffer, "api.met.no") != NULL;
    weather_failure = mode && !strcmp(mode, "weather-fail");
    if (weather_open_route || weather_met_route) {
        if (weather_count) {
            FILE *count_file = fopen(weather_count, "r+");
            unsigned long count = 0;

            if (count_file) {
                if (fscanf(count_file, "%lu", &count) != 1)
                    count = 0;
                rewind(count_file);
                fprintf(count_file, "%lu\n", count + 1);
                fclose(count_file);
            }
        }
        if (weather_failure) {
            fputs("{\"error\":\"weather unavailable\"}\n", stdout);
            status = 503;
        } else if (weather_open_route) {
            fputs("{\"current\":{\"temperature_2m\":72.4,"
                  "\"weather_code\":1,\"wind_speed_10m\":5.6}}\n",
                  stdout);
        } else {
            fputs("{\"properties\":{\"timeseries\":[{\"data\":{"
                  "\"instant\":{\"details\":{\"air_temperature\":12.5,"
                  "\"wind_speed\":3.1}},\"next_1_hours\":{\"summary\":{"
                  "\"symbol_code\":\"partlycloudy_day\"}}}}}", stdout);
            if (mode && !strcmp(mode, "weather-met-no-large")) {
                for (i = 0; i < 20000; ++i)
                    fputc(' ', stdout);
            }
            fputs("]}}\n", stdout);
        }
    } else if (body_capture) {
        FILE *body_output = fopen(body_capture, "w");

        if (body_output) {
            fputs(body, body_output);
            fclose(body_output);
        }
    }
    if ((mode && !strcmp(mode, "sse")) ||
        (!mode && response_route) ||
        (mode && !strncmp(mode, "weather-", 8) && response_route)) {
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
    fprintf(stdout, "\n__LE_HTTP_STATUS__:%d\n", status);
    return 0;
}
