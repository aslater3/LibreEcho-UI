#include "adapter/llm_provider.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    const struct le_llm_provider *provider =
        le_llm_provider_by_id("openai-codex");
    struct le_llm_http_request request;
    struct le_llm_auth_session session;
    struct le_llm_credentials credentials;
    char delta[256];

    CHECK(provider != NULL);
    CHECK(provider->subscription_auth);
    CHECK(le_llm_provider_by_id("metered-openai-api") == NULL);
    CHECK(provider->auth_start_request(&request) == 0);
    CHECK(!strcmp(request.url,
                  "https://auth.openai.com/api/accounts/deviceauth/usercode"));
    CHECK(strstr(request.body, "app_EMoamEEZ73f0CkXaXp7hrann") != NULL);
    CHECK(strstr(request.url, "api.openai.com") == NULL);

    CHECK(provider->auth_start_response(
              200,
              "{\"user_code\":\"ABCD-EFGH\","
              "\"device_auth_id\":\"device-123\",\"interval\":2}",
              &session) == 0);
    CHECK(!strcmp(session.user_code, "ABCD-EFGH"));
    CHECK(!strcmp(session.verification_url,
                  "https://auth.openai.com/codex/device"));
    CHECK(session.interval_seconds == 3);
    CHECK(provider->auth_poll_request(&session, &request) == 0);
    CHECK(strstr(request.body, "\"device_auth_id\":\"device-123\"") != NULL);
    CHECK(provider->auth_poll_response(403, "{}", &session) == 0);
    CHECK(provider->auth_poll_response(
              200,
              "{\"authorization_code\":\"auth/code\","
              "\"code_verifier\":\"verify-value\"}",
              &session) == 1);
    CHECK(provider->token_exchange_request(&session, &request) == 0);
    CHECK(strstr(request.body, "code=auth%2Fcode") != NULL);
    CHECK(strstr(request.body, "grant_type=authorization_code") != NULL);

    memset(&credentials, 0, sizeof(credentials));
    CHECK(provider->token_response(
              200,
              "{\"access_token\":\"header."
              "eyJleHAiOjE5MDAwMDAwMDAsImh0dHBzOi8vYXBpLm9wZW5haS5jb20v"
              "YXV0aCI6eyJjaGF0Z3B0X2FjY291bnRfaWQiOiJhY2N0LTEyMyJ9fQ."
              "signature\",\"refresh_token\":\"refresh-one\"}",
              &credentials) == 0);
    CHECK(!strcmp(credentials.account_id, "acct-123"));
    CHECK(credentials.expires_at == (time_t)1900000000);
    CHECK(provider->refresh_request(&credentials, &request) == 0);
    CHECK(strstr(request.body, "refresh_token=refresh-one") != NULL);

    CHECK(provider->response_request(
              &credentials, "gpt-5.4",
              le_llm_default_voice_prompt(),
              "What's the weather?", &request) == 0);
    CHECK(!strcmp(request.url,
                  "https://chatgpt.com/backend-api/codex/responses"));
    CHECK(strstr(request.url, "api.openai.com") == NULL);
    CHECK(!strcmp(request.account_id, "acct-123"));
    CHECK(strstr(request.body, "\"store\":false") != NULL);
    CHECK(strstr(request.body, "\"stream\":true") != NULL);
    CHECK(strstr(request.body, "\"effort\":\"low\"") != NULL);
    CHECK(strstr(request.body, "natural spoken English") != NULL);

    CHECK(provider->stream_event(
              "{\"type\":\"response.output_text.delta\","
              "\"delta\":\"Hello, Bonnie \\u2014 welcome home.\"}",
              delta, sizeof(delta)) == LE_LLM_STREAM_DELTA);
    CHECK(!strcmp(delta, "Hello, Bonnie \xe2\x80\x94 welcome home."));
    CHECK(provider->stream_event(
              "{\"type\":\"response.completed\"}",
              delta, sizeof(delta)) == LE_LLM_STREAM_COMPLETE);
    CHECK(provider->stream_event(
              "{\"type\":\"response.failed\"}",
              delta, sizeof(delta)) == LE_LLM_STREAM_ERROR);

    provider = le_llm_provider_by_id("openai-compatible");
    CHECK(provider != NULL);
    CHECK(!provider->subscription_auth);
    memset(&credentials, 0, sizeof(credentials));
    strcpy(credentials.base_url, "http://192.168.10.20:8000/v1");
    strcpy(credentials.api_key, "local-key");
    CHECK(provider->response_request(
              &credentials, "gemma-4", "Be concise.", "Hello?",
              &request) == 0);
    CHECK(!strcmp(request.url,
                  "http://192.168.10.20:8000/v1/chat/completions"));
    CHECK(request.allow_insecure_http);
    CHECK(!strcmp(request.authorization, "Bearer local-key"));
    CHECK(strstr(request.body, "\"role\":\"system\"") != NULL);
    CHECK(strstr(request.body, "\"stream\":true") != NULL);
    CHECK(provider->stream_event(
              "{\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}",
              delta, sizeof(delta)) == LE_LLM_STREAM_DELTA);
    CHECK(!strcmp(delta, "Hi"));
    CHECK(provider->stream_event("[DONE]", delta, sizeof(delta)) ==
          LE_LLM_STREAM_COMPLETE);

    puts("llm provider: subscription and local-compatible contracts: ok");
    return 0;
}
