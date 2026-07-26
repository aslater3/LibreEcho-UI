#define _POSIX_C_SOURCE 200809L

#include "llm_provider.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODEX_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_AUTH_BASE "https://auth.openai.com"
#define CODEX_RESPONSES_URL \
    "https://chatgpt.com/backend-api/codex/responses"

static int copy_text(char *output, size_t size, const char *value)
{
    size_t length;

    if (!output || !size || !value)
        return -1;
    length = strlen(value);
    if (length >= size)
        return -1;
    memcpy(output, value, length + 1);
    return 0;
}

static const char *find_json_value(const char *json, const char *key)
{
    char needle[128];
    const char *position;

    if (!json || !key ||
        snprintf(needle, sizeof(needle), "\"%s\"", key) >=
            (int)sizeof(needle))
        return NULL;
    position = json;
    while ((position = strstr(position, needle)) != NULL) {
        const char *value = position + strlen(needle);

        while (isspace((unsigned char)*value))
            ++value;
        if (*value++ != ':') {
            position += strlen(needle);
            continue;
        }
        while (isspace((unsigned char)*value))
            ++value;
        return value;
    }
    return NULL;
}

static int append_utf8(char *output, size_t size, size_t *used,
                       unsigned long codepoint)
{
    unsigned char encoded[4];
    size_t count;

    if (codepoint <= 0x7fUL) {
        encoded[0] = (unsigned char)codepoint;
        count = 1;
    } else if (codepoint <= 0x7ffUL) {
        encoded[0] = (unsigned char)(0xc0U | (codepoint >> 6));
        encoded[1] = (unsigned char)(0x80U | (codepoint & 0x3fU));
        count = 2;
    } else if (codepoint <= 0xffffUL) {
        if (codepoint >= 0xd800UL && codepoint <= 0xdfffUL)
            return -1;
        encoded[0] = (unsigned char)(0xe0U | (codepoint >> 12));
        encoded[1] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3fU));
        encoded[2] = (unsigned char)(0x80U | (codepoint & 0x3fU));
        count = 3;
    } else if (codepoint <= 0x10ffffUL) {
        encoded[0] = (unsigned char)(0xf0U | (codepoint >> 18));
        encoded[1] = (unsigned char)(0x80U | ((codepoint >> 12) & 0x3fU));
        encoded[2] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3fU));
        encoded[3] = (unsigned char)(0x80U | (codepoint & 0x3fU));
        count = 4;
    } else {
        return -1;
    }
    if (*used + count >= size)
        return -1;
    memcpy(output + *used, encoded, count);
    *used += count;
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex4(const char *input, unsigned long *value)
{
    unsigned long parsed = 0;
    size_t i;

    for (i = 0; i < 4; ++i) {
        int digit = hex_value(input[i]);

        if (digit < 0)
            return -1;
        parsed = (parsed << 4) | (unsigned long)digit;
    }
    *value = parsed;
    return 0;
}

static int json_string(const char *json, const char *key,
                       char *output, size_t size)
{
    const char *value = find_json_value(json, key);
    size_t used = 0;

    if (!value)
        return 0;
    if (*value++ != '"')
        return -1;
    while (*value && *value != '"') {
        unsigned char c = (unsigned char)*value++;

        if (c == '\\') {
            unsigned long codepoint;

            c = (unsigned char)*value++;
            if (c == '"' || c == '\\' || c == '/')
                ;
            else if (c == 'b')
                c = '\b';
            else if (c == 'f')
                c = '\f';
            else if (c == 'n')
                c = '\n';
            else if (c == 'r')
                c = '\r';
            else if (c == 't')
                c = '\t';
            else if (c == 'u') {
                if (parse_hex4(value, &codepoint) < 0)
                    return -1;
                value += 4;
                if (codepoint >= 0xd800UL && codepoint <= 0xdbffUL &&
                    value[0] == '\\' && value[1] == 'u') {
                    unsigned long low;

                    if (parse_hex4(value + 2, &low) < 0 ||
                        low < 0xdc00UL || low > 0xdfffUL)
                        return -1;
                    codepoint = 0x10000UL +
                        ((codepoint - 0xd800UL) << 10) +
                        (low - 0xdc00UL);
                    value += 6;
                }
                if (append_utf8(output, size, &used, codepoint) < 0)
                    return -1;
                continue;
            } else {
                return -1;
            }
        }
        if (c < 0x20U || used + 1 >= size)
            return -1;
        output[used++] = (char)c;
    }
    if (*value != '"' || used >= size)
        return -1;
    output[used] = '\0';
    return 1;
}

static int json_unsigned(const char *json, const char *key,
                         unsigned long long *output)
{
    const char *value = find_json_value(json, key);
    char *end;
    unsigned long long parsed;

    if (!value)
        return 0;
    parsed = strtoull(value, &end, 10);
    if (end == value)
        return -1;
    *output = parsed;
    return 1;
}

static int json_escape(char *output, size_t size, const char *input)
{
    static const char hex[] = "0123456789abcdef";
    size_t used = 0;

    if (!output || !size || !input)
        return -1;
    while (*input) {
        unsigned char c = (unsigned char)*input++;

        if (c == '"' || c == '\\') {
            if (used + 2 >= size)
                return -1;
            output[used++] = '\\';
            output[used++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            if (used + 2 >= size)
                return -1;
            output[used++] = '\\';
            output[used++] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
        } else if (c < 0x20U) {
            if (used + 6 >= size)
                return -1;
            output[used++] = '\\';
            output[used++] = 'u';
            output[used++] = '0';
            output[used++] = '0';
            output[used++] = hex[c >> 4];
            output[used++] = hex[c & 15U];
        } else {
            if (used + 1 >= size)
                return -1;
            output[used++] = (char)c;
        }
    }
    output[used] = '\0';
    return 0;
}

static int url_encode(char *output, size_t size, const char *input)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;

    while (input && *input) {
        unsigned char c = (unsigned char)*input++;

        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (used + 1 >= size)
                return -1;
            output[used++] = (char)c;
        } else {
            if (used + 3 >= size)
                return -1;
            output[used++] = '%';
            output[used++] = hex[c >> 4];
            output[used++] = hex[c & 15U];
        }
    }
    if (used >= size)
        return -1;
    output[used] = '\0';
    return 0;
}

static int base64url_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '-' || c == '+')
        return 62;
    if (c == '_' || c == '/')
        return 63;
    return -1;
}

static int jwt_payload(const char *token, char *output, size_t size)
{
    const char *start;
    const char *end;
    unsigned int accumulator = 0;
    unsigned int bits = 0;
    size_t used = 0;

    if (!token || !(start = strchr(token, '.')))
        return -1;
    ++start;
    end = strchr(start, '.');
    if (!end)
        return -1;
    while (start < end) {
        int value = base64url_value(*start++);

        if (value < 0)
            return -1;
        accumulator = (accumulator << 6) | (unsigned int)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (used + 1 >= size)
                return -1;
            output[used++] = (char)((accumulator >> bits) & 0xffU);
        }
    }
    output[used] = '\0';
    return 0;
}

static void populate_claims(struct le_llm_credentials *credentials)
{
    char payload[LE_LLM_TOKEN_MAX];
    unsigned long long expiry;

    credentials->account_id[0] = '\0';
    credentials->expires_at = 0;
    if (jwt_payload(credentials->access_token,
                    payload, sizeof(payload)) < 0)
        return;
    (void)json_string(payload, "chatgpt_account_id",
                      credentials->account_id,
                      sizeof(credentials->account_id));
    if (json_unsigned(payload, "exp", &expiry) > 0)
        credentials->expires_at = (time_t)expiry;
}

static void request_init(struct le_llm_http_request *request,
                         const char *url, const char *content_type)
{
    memset(request, 0, sizeof(*request));
    (void)copy_text(request->method, sizeof(request->method), "POST");
    (void)copy_text(request->url, sizeof(request->url), url);
    (void)copy_text(request->content_type, sizeof(request->content_type),
                    content_type);
}

static int auth_start_request(struct le_llm_http_request *request)
{
    if (!request)
        return -1;
    request_init(request,
                 CODEX_AUTH_BASE "/api/accounts/deviceauth/usercode",
                 "application/json");
    return snprintf(request->body, sizeof(request->body),
                    "{\"client_id\":\"%s\"}", CODEX_CLIENT_ID) <
            (int)sizeof(request->body) ? 0 : -1;
}

static int auth_start_response(int status, const char *body,
                               struct le_llm_auth_session *session)
{
    unsigned long long interval = 5;
    unsigned long long expires = 900;

    if (status != 200 || !body || !session)
        return -1;
    memset(session, 0, sizeof(*session));
    if (json_string(body, "user_code", session->user_code,
                    sizeof(session->user_code)) < 1 ||
        json_string(body, "device_auth_id", session->device_auth_id,
                    sizeof(session->device_auth_id)) < 1)
        return -1;
    (void)json_unsigned(body, "interval", &interval);
    (void)json_unsigned(body, "expires_in", &expires);
    if (interval < 3)
        interval = 3;
    if (interval > 30)
        interval = 30;
    if (expires < 60 || expires > 1800)
        expires = 900;
    session->interval_seconds = (unsigned int)interval;
    session->expires_at = time(NULL) + (time_t)expires;
    return copy_text(session->verification_url,
                     sizeof(session->verification_url),
                     CODEX_AUTH_BASE "/codex/device");
}

static int auth_poll_request(const struct le_llm_auth_session *session,
                             struct le_llm_http_request *request)
{
    char device[1024];
    char code[192];

    if (!session || !request ||
        json_escape(device, sizeof(device), session->device_auth_id) < 0 ||
        json_escape(code, sizeof(code), session->user_code) < 0)
        return -1;
    request_init(request,
                 CODEX_AUTH_BASE "/api/accounts/deviceauth/token",
                 "application/json");
    return snprintf(request->body, sizeof(request->body),
                    "{\"device_auth_id\":\"%s\",\"user_code\":\"%s\"}",
                    device, code) < (int)sizeof(request->body) ? 0 : -1;
}

static int auth_poll_response(int status, const char *body,
                              struct le_llm_auth_session *session)
{
    if (!session)
        return -1;
    if (status == 403 || status == 404)
        return 0;
    if (status != 200 || !body)
        return -1;
    if (json_string(body, "authorization_code",
                    session->authorization_code,
                    sizeof(session->authorization_code)) < 1 ||
        json_string(body, "code_verifier", session->code_verifier,
                    sizeof(session->code_verifier)) < 1)
        return -1;
    return 1;
}

static int token_exchange_request(const struct le_llm_auth_session *session,
                                  struct le_llm_http_request *request)
{
    char code[8192];
    char verifier[1024];

    if (!session || !request ||
        url_encode(code, sizeof(code), session->authorization_code) < 0 ||
        url_encode(verifier, sizeof(verifier), session->code_verifier) < 0)
        return -1;
    request_init(request, CODEX_AUTH_BASE "/oauth/token",
                 "application/x-www-form-urlencoded");
    return snprintf(
        request->body, sizeof(request->body),
        "grant_type=authorization_code&code=%s&"
        "redirect_uri=https%%3A%%2F%%2Fauth.openai.com%%2Fdeviceauth%%2Fcallback&"
        "client_id=%s&code_verifier=%s",
        code, CODEX_CLIENT_ID, verifier) < (int)sizeof(request->body) ? 0 : -1;
}

static int refresh_request(const struct le_llm_credentials *credentials,
                           struct le_llm_http_request *request)
{
    char token[LE_LLM_TOKEN_MAX * 3U];

    if (!credentials || !credentials->refresh_token[0] || !request ||
        url_encode(token, sizeof(token), credentials->refresh_token) < 0)
        return -1;
    request_init(request, CODEX_AUTH_BASE "/oauth/token",
                 "application/x-www-form-urlencoded");
    return snprintf(request->body, sizeof(request->body),
                    "grant_type=refresh_token&refresh_token=%s&client_id=%s",
                    token, CODEX_CLIENT_ID) <
            (int)sizeof(request->body) ? 0 : -1;
}

static int token_response(int status, const char *body,
                          struct le_llm_credentials *credentials)
{
    char access[LE_LLM_TOKEN_MAX];
    char refresh[LE_LLM_TOKEN_MAX];
    int refresh_result;

    if (status != 200 || !body || !credentials ||
        json_string(body, "access_token", access, sizeof(access)) < 1)
        return -1;
    refresh_result =
        json_string(body, "refresh_token", refresh, sizeof(refresh));
    if (refresh_result < 0 ||
        (refresh_result == 0 && !credentials->refresh_token[0]))
        return -1;
    if (copy_text(credentials->access_token,
                  sizeof(credentials->access_token), access) < 0)
        return -1;
    if (refresh_result > 0 &&
        copy_text(credentials->refresh_token,
                  sizeof(credentials->refresh_token), refresh) < 0)
        return -1;
    populate_claims(credentials);
    memset(access, 0, sizeof(access));
    memset(refresh, 0, sizeof(refresh));
    return 0;
}

static int response_request(const struct le_llm_credentials *credentials,
                            const char *model, const char *instructions,
                            const char *transcript,
                            struct le_llm_http_request *request)
{
    char escaped_model[256];
    char escaped_instructions[LE_LLM_TEXT_MAX * 2U];
    char escaped_transcript[LE_LLM_TEXT_MAX * 2U];
    int length;

    if (!credentials || !credentials->access_token[0] ||
        !model || !model[0] || !instructions || !transcript ||
        !transcript[0] || !request ||
        json_escape(escaped_model, sizeof(escaped_model), model) < 0 ||
        json_escape(escaped_instructions, sizeof(escaped_instructions),
                    instructions) < 0 ||
        json_escape(escaped_transcript, sizeof(escaped_transcript),
                    transcript) < 0)
        return -1;
    request_init(request, CODEX_RESPONSES_URL, "application/json");
    if (snprintf(request->authorization,
                 sizeof(request->authorization), "Bearer %s",
                 credentials->access_token) >=
        (int)sizeof(request->authorization) ||
        copy_text(request->account_id, sizeof(request->account_id),
                  credentials->account_id) < 0)
        return -1;
    request->accept_sse = 1;
    length = snprintf(
        request->body, sizeof(request->body),
        "{\"model\":\"%s\",\"instructions\":\"%s\","
        "\"input\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"input_text\",\"text\":\"%s\"}]}],"
        "\"store\":false,\"stream\":true,"
        "\"reasoning\":{\"effort\":\"low\"},\"max_output_tokens\":160}",
        escaped_model, escaped_instructions, escaped_transcript);
    return length > 0 && length < (int)sizeof(request->body) ? 0 : -1;
}

static int stream_event(const char *data, char *delta, size_t delta_size)
{
    char type[96];
    int result;

    if (!data || !delta || !delta_size)
        return LE_LLM_STREAM_ERROR;
    delta[0] = '\0';
    result = json_string(data, "type", type, sizeof(type));
    if (result < 1)
        return LE_LLM_STREAM_IGNORED;
    if (!strcmp(type, "response.output_text.delta")) {
        return json_string(data, "delta", delta, delta_size) > 0
            ? LE_LLM_STREAM_DELTA : LE_LLM_STREAM_ERROR;
    }
    if (!strcmp(type, "response.completed"))
        return LE_LLM_STREAM_COMPLETE;
    if (!strcmp(type, "response.failed") ||
        !strcmp(type, "response.incomplete") ||
        !strcmp(type, "error"))
        return LE_LLM_STREAM_ERROR;
    return LE_LLM_STREAM_IGNORED;
}

static const struct le_llm_provider provider = {
    "openai-codex",
    "ChatGPT",
    1,
    auth_start_request,
    auth_start_response,
    auth_poll_request,
    auth_poll_response,
    token_exchange_request,
    refresh_request,
    token_response,
    response_request,
    stream_event
};

const struct le_llm_provider *le_llm_codex_provider(void)
{
    return &provider;
}
