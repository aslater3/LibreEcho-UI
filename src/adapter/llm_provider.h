#ifndef LIBREECHO_LLM_PROVIDER_H
#define LIBREECHO_LLM_PROVIDER_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define LE_LLM_TOKEN_MAX 8192
#define LE_LLM_BODY_MAX 16384
#define LE_LLM_TEXT_MAX 4096
#define LE_LLM_URL_MAX 384

struct le_llm_http_request {
    char method[8];
    char url[LE_LLM_URL_MAX];
    char content_type[80];
    char authorization[LE_LLM_TOKEN_MAX + 16];
    char account_id[160];
    char body[LE_LLM_BODY_MAX];
    int accept_sse;
    /* Set only after endpoint validation permits a configured LAN HTTP URL. */
    int allow_insecure_http;
};

struct le_llm_auth_session {
    char user_code[96];
    char verification_url[LE_LLM_URL_MAX];
    char device_auth_id[512];
    char authorization_code[4096];
    char code_verifier[512];
    unsigned int interval_seconds;
    time_t expires_at;
};

struct le_llm_credentials {
    char access_token[LE_LLM_TOKEN_MAX];
    char refresh_token[LE_LLM_TOKEN_MAX];
    char api_key[LE_LLM_TOKEN_MAX];
    char base_url[LE_LLM_URL_MAX];
    char account_id[160];
    time_t expires_at;
};

enum le_llm_stream_result {
    LE_LLM_STREAM_ERROR = -1,
    LE_LLM_STREAM_IGNORED = 0,
    LE_LLM_STREAM_DELTA = 1,
    LE_LLM_STREAM_COMPLETE = 2
};

struct le_llm_provider {
    const char *id;
    const char *name;
    int subscription_auth;

    int (*auth_start_request)(struct le_llm_http_request *request);
    int (*auth_start_response)(int status, const char *body,
                               struct le_llm_auth_session *session);
    int (*auth_poll_request)(const struct le_llm_auth_session *session,
                             struct le_llm_http_request *request);
    int (*auth_poll_response)(int status, const char *body,
                              struct le_llm_auth_session *session);
    int (*token_exchange_request)(const struct le_llm_auth_session *session,
                                  struct le_llm_http_request *request);
    int (*refresh_request)(const struct le_llm_credentials *credentials,
                           struct le_llm_http_request *request);
    int (*token_response)(int status, const char *body,
                          struct le_llm_credentials *credentials);
    int (*response_request)(const struct le_llm_credentials *credentials,
                            const char *model, const char *instructions,
                            const char *transcript,
                            struct le_llm_http_request *request);
    int (*stream_event)(const char *data, char *delta, size_t delta_size);
};

const struct le_llm_provider *le_llm_provider_by_id(const char *id);
const char *le_llm_default_voice_prompt(void);

#endif
