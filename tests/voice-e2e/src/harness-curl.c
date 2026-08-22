/*
 * Hermetic stand-in for curl, used as agentd's --curl helper.
 *
 * agentd writes a curl config file (llm_http.c) and pipes the request body on
 * stdin; it expects the response body on stdout followed by
 * "\n__LE_HTTP_STATUS__:<code>\n".  We append every request to
 * $LE_HARNESS_LLM_LOG so the harness can assert the model actually saw the
 * transcript, and answer with a reply the harness can recognise.
 *
 * Unlike tests/mock_llm_curl.c this appends rather than truncates (a voice
 * turn makes several calls) and the reply is derived from the request, so a
 * pipeline that dispatches the wrong text cannot pass.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *log_path = getenv("LE_HARNESS_LLM_LOG");
    const char *reply = getenv("LE_HARNESS_LLM_REPLY");
    char config[16384];
    char body[65536];
    size_t config_used = 0;
    size_t body_used = 0;
    FILE *input;
    int i;

    for (i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--config"))
            config_path = argv[i + 1];
    if (!config_path)
        return 2;
    input = fopen(config_path, "r");
    if (!input)
        return 3;
    config_used = fread(config, 1, sizeof(config) - 1, input);
    config[config_used] = '\0';
    fclose(input);
    while (body_used + 1 < sizeof(body)) {
        size_t count = fread(body + body_used, 1,
                             sizeof(body) - body_used - 1, stdin);

        if (!count)
            break;
        body_used += count;
    }
    body[body_used] = '\0';
    if (log_path && *log_path) {
        FILE *log = fopen(log_path, "a");

        if (log) {
            fprintf(log, "%s\n", body);
            fclose(log);
        }
    }
    if (!reply || !*reply)
        reply = "It is four thirty two in the afternoon.";
    /* openai-compatible (/v1/chat/completions) buffered form. */
    printf("{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
           "\"message\":{\"role\":\"assistant\",\"content\":\"%s\"}}]}\n",
           reply);
    printf("\n__LE_HTTP_STATUS__:200\n");
    return 0;
}
