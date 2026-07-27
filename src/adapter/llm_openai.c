#define _POSIX_C_SOURCE 200809L

#include "llm_provider.h"

#include <stdio.h>
#include <string.h>

static int copy_text(char *output, size_t size, const char *value)
{
    size_t length = value ? strlen(value) : 0;

    if (!output || !size || !value || length >= size)
        return -1;
    memcpy(output, value, length + 1);
    return 0;
}

static int json_escape(char *output, size_t size, const char *input)
{
    size_t used = 0;

    while (input && *input) {
        unsigned char c = (unsigned char)*input++;

        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
            if (used + 2 >= size)
                return -1;
            output[used++] = '\\';
            output[used++] = c == '\n' ? 'n' : c == '\r' ? 'r' :
                             c == '\t' ? 't' : (char)c;
        } else if (c < 0x20U) {
            return -1;
        } else {
            if (used + 1 >= size)
                return -1;
            output[used++] = (char)c;
        }
    }
    if (used >= size)
        return -1;
    output[used] = '\0';
    return 0;
}

static int json_string(const char *json, const char *key,
                       char *output, size_t size)
{
    char needle[96];
    const char *value;
    size_t used = 0;

    if (!json || !key || snprintf(needle, sizeof(needle), "\"%s\":\"", key) >=
        (int)sizeof(needle) || !(value = strstr(json, needle)))
        return 0;
    value += strlen(needle);
    while (*value && *value != '"') {
        unsigned char c = (unsigned char)*value++;

        if (c == '\\') {
            c = (unsigned char)*value++;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c != '"' && c != '\\' && c != '/') return -1;
        }
        if (used + 1 >= size)
            return -1;
        output[used++] = (char)c;
    }
    if (*value != '"' || used >= size)
        return -1;
    output[used] = '\0';
    return 1;
}

static int response_request(const struct le_llm_credentials *credentials,
                            const char *model, const char *instructions,
                            const char *transcript,
                            struct le_llm_http_request *request)
{
    char url[LE_LLM_URL_MAX];
    char escaped_model[256], escaped_prompt[LE_LLM_TEXT_MAX * 2U];
    char escaped_transcript[LE_LLM_TEXT_MAX * 2U];
    int length;

    if (!credentials || !credentials->base_url[0] || !model || !model[0] ||
        !instructions || !transcript || !transcript[0] || !request ||
        json_escape(escaped_model, sizeof(escaped_model), model) < 0 ||
        json_escape(escaped_prompt, sizeof(escaped_prompt), instructions) < 0 ||
        json_escape(escaped_transcript, sizeof(escaped_transcript), transcript) < 0)
        return -1;
    if (snprintf(url, sizeof(url), "%s%s",
                 credentials->base_url,
                 strstr(credentials->base_url, "/chat/completions")
                     ? "" : "/chat/completions") >= (int)sizeof(url))
        return -1;
    memset(request, 0, sizeof(*request));
    copy_text(request->method, sizeof(request->method), "POST");
    copy_text(request->url, sizeof(request->url), url);
    copy_text(request->content_type, sizeof(request->content_type),
              "application/json");
    if (credentials->api_key[0] && snprintf(request->authorization,
            sizeof(request->authorization), "Bearer %s", credentials->api_key) >=
            (int)sizeof(request->authorization))
        return -1;
    request->allow_insecure_http = !strncmp(url, "http://", 7);
    request->accept_sse = 1;
    length = snprintf(request->body, sizeof(request->body),
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"system\",\"content\":\"%s\"},{\"role\":\"user\",\"content\":\"%s\"}],\"stream\":true}",
        escaped_model, escaped_prompt, escaped_transcript);
    return length > 0 && length < (int)sizeof(request->body) ? 0 : -1;
}

static int stream_event(const char *data, char *delta, size_t delta_size)
{
    char content[1024];

    if (!data || !delta || !delta_size)
        return LE_LLM_STREAM_ERROR;
    delta[0] = '\0';
    if (!strcmp(data, "[DONE]"))
        return LE_LLM_STREAM_COMPLETE;
    if (json_string(data, "content", content, sizeof(content)) > 0) {
        if (copy_text(delta, delta_size, content) < 0)
            return LE_LLM_STREAM_ERROR;
        return LE_LLM_STREAM_DELTA;
    }
    if (strstr(data, "\"finish_reason\":\"stop\""))
        return LE_LLM_STREAM_COMPLETE;
    if (strstr(data, "\"error\""))
        return LE_LLM_STREAM_ERROR;
    return LE_LLM_STREAM_IGNORED;
}

static const struct le_llm_provider provider = {
    "openai-compatible", "OpenAI-compatible local endpoint", 0,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, response_request, stream_event
};

const struct le_llm_provider *le_llm_openai_provider(void)
{
    return &provider;
}
