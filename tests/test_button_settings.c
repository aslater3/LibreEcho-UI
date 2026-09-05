#include "../src/api.h"
#include "../src/config_store.h"
#include "../src/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static const char *config_path = "/tmp/libreecho-button-settings-test.json";
static const char csrf[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } else { \
        printf("ok: %s\n", message); \
    } \
} while (0)

static int request(struct api_context *context, const char *method,
                   const char *body, struct api_response *response)
{
    struct api_request query;

    memset(&query, 0, sizeof(query));
    snprintf(query.method, sizeof(query.method), "%s", method);
    snprintf(query.path, sizeof(query.path), "/api/v1/buttons");
    snprintf(query.csrf, sizeof(query.csrf), "%s", csrf);
    query.body = body ? body : "";
    query.body_len = strlen(query.body);
    memset(response, 0, sizeof(*response));
    api_handle(context, &query, response);
    return response->status;
}

static int init_context(struct api_context *context, struct le_backend **backend)
{
    unlink(config_path);
    unlink("/tmp/libreecho-button-settings-test.json.bak");
    if (le_backend_init(backend, "mock", NULL, NULL, 7) != LE_OK)
        return 0;
    if (api_init(context, *backend, 1, 1, NULL, NULL, csrf,
                 config_path, NULL) != 0) {
        le_backend_destroy(*backend);
        return 0;
    }
    return 1;
}

static void expect_invalid_without_mutation(struct api_context *context,
                                             const char *body,
                                             const char *message)
{
    struct api_response response;
    char action[sizeof(context->button_action)];
    char sounds[sizeof(context->button_action_sounds)];
    int tones = context->button_tones;
    int action_brightness = context->button_action_brightness;
    int mute_brightness = context->button_mute_brightness;

    snprintf(action, sizeof(action), "%s", context->button_action);
    snprintf(sounds, sizeof(sounds), "%s", context->button_action_sounds);
    CHECK(request(context, "PUT", body, &response) == 400, message);
    CHECK(tones == context->button_tones &&
          action_brightness == context->button_action_brightness &&
          mute_brightness == context->button_mute_brightness &&
          !strcmp(action, context->button_action) &&
          !strcmp(sounds, context->button_action_sounds),
          "invalid button settings do not mutate context");
}

int main(void)
{
    struct api_context context, restarted, corrupt;
    struct le_backend *backend = NULL, *restarted_backend = NULL;
    struct le_backend *corrupt_backend = NULL;
    struct api_response response;
    char body[512], oversized[220];

    CHECK(init_context(&context, &backend), "initialise mock API context");
    if (!backend)
        return 1;

    CHECK(request(&context, "GET", NULL, &response) == 200,
          "GET button settings succeeds");
    CHECK(json_valid_object(response.body, response.length),
          "GET button response is valid JSON");
    CHECK(strstr(response.body, "\"action\":\"sound\"") != NULL,
          "GET exposes default action setting");

    CHECK(request(&context, "PUT",
                  "{\"tones\":false,\"action\":\"disabled\","
                  "\"action_sounds\":\"action-3,action-1\","
                  "\"action_brightness\":11,\"mute_brightness\":22}",
                  &response) == 200,
          "full button settings update succeeds");
    CHECK(context.button_tones == 0 && !strcmp(context.button_action, "disabled") &&
          !strcmp(context.button_action_sounds, "action-3,action-1") &&
          context.button_action_brightness == 11 &&
          context.button_mute_brightness == 22,
          "full button settings update is applied");
    { char saved[4096]; int value = -1;
      CHECK(config_read(config_path, saved, sizeof(saved)) > 0 &&
            json_get_bool(saved, "button_tones", &value) == 1 && value == 0 &&
            strstr(saved, "action-3,action-1") != NULL,
            "PUT persists button settings without an explicit save");
    }

    CHECK(request(&context, "PUT", "{\"action_brightness\":33}",
                  &response) == 200,
          "partial brightness update succeeds");
    CHECK(context.button_tones == 0 && !strcmp(context.button_action, "disabled") &&
          !strcmp(context.button_action_sounds, "action-3,action-1") &&
          context.button_action_brightness == 33 &&
          context.button_mute_brightness == 22,
          "partial update preserves unspecified settings");
    CHECK(request(&context, "PUT", "{\"tones\":true}", &response) == 200,
          "partial boolean update succeeds");
    CHECK(context.button_tones == 1 && context.button_action_brightness == 33,
          "partial boolean update preserves other settings");

    expect_invalid_without_mutation(&context, "{}",
                                    "missing button settings are rejected");
    expect_invalid_without_mutation(&context, "{\"tones\":\"true\"}",
                                    "boolean type mismatch is rejected");
    expect_invalid_without_mutation(&context,
                                    "{\"action\":true}",
                                    "action type mismatch is rejected");
    expect_invalid_without_mutation(&context,
                                    "{\"action_brightness\":\"44\"}",
                                    "brightness type mismatch is rejected");
    expect_invalid_without_mutation(&context,
                                    "{\"mute_brightness\":101}",
                                    "brightness above range is rejected");
    expect_invalid_without_mutation(&context,
                                    "{\"action\":\"other\"}",
                                    "unknown action is rejected");
    expect_invalid_without_mutation(&context,
                                    "{\"action_sounds\":\"action-1,action-4\"}",
                                    "unknown sound entry is rejected");
    expect_invalid_without_mutation(&context,
                                    "{\"action_sounds\":\"action-1,,action-2\"}",
                                    "empty sound entry is rejected");
    expect_invalid_without_mutation(&context,
                                    "{\"action_sounds\":\"action-1/foo\"}",
                                    "slash in sound entry is rejected");
    expect_invalid_without_mutation(&context,
                                    "{\"action_sounds\":\"action-1\\\"bad\"}",
                                    "quote in sound entry is rejected");

    memset(oversized, 'a', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    snprintf(body, sizeof(body), "{\"action_sounds\":\"%s\"}", oversized);
    expect_invalid_without_mutation(&context, body,
                                    "oversized sound list is rejected");

    CHECK(request(&context, "PUT", "{\"action_sounds\":\"\"}",
                  &response) == 200,
          "empty sound list is accepted");
    CHECK(!context.button_action_sounds[0], "empty sound list is stored");
    CHECK(request(&context, "PUT",
                  "{\"short_press\":\"say \\\"hi\\\"\","
                  "\"long_press\":\"path\\\\test\"}", &response) == 200,
          "legacy short and long fields remain accepted");
    CHECK(!strcmp(context.button_short, "say \"hi\"") &&
          !strcmp(context.button_long, "path\\test"),
          "legacy button fields retain decoded values");
    CHECK(strstr(response.body, "say \\\"hi\\\"") != NULL &&
          strstr(response.body, "path\\\\test") != NULL,
          "legacy button fields are escaped in GET response");

    { char original_path[sizeof(context.config_path)];
      snprintf(original_path, sizeof(original_path), "%s", context.config_path);
      snprintf(context.config_path, sizeof(context.config_path), "%s/child", config_path);
      CHECK(request(&context, "PUT", "{\"tones\":false}", &response) == 503,
            "failed persistence returns 503");
      CHECK(context.button_tones == 1, "failed persistence preserves context");
      snprintf(context.config_path, sizeof(context.config_path), "%s", original_path);
    }
    CHECK(le_backend_init(&restarted_backend, "mock", NULL, NULL, 8) == LE_OK &&
          api_init(&restarted, restarted_backend, 1, 1, NULL, NULL, csrf,
                   config_path, NULL) == 0,
          "restart context from persisted settings");
    CHECK(restarted.button_tones == 1 && !strcmp(restarted.button_action, "disabled") &&
          !restarted.button_action_sounds[0] &&
          restarted.button_action_brightness == 33 &&
          restarted.button_mute_brightness == 22 &&
          !strcmp(restarted.button_short, "say \"hi\"") &&
          !strcmp(restarted.button_long, "path\\test"),
          "restart restores button settings and legacy fields");
    request(&restarted, "GET", NULL, &response);
    CHECK(json_valid_object(response.body, response.length),
          "restarted GET response is valid JSON");

    CHECK(config_write_atomic(config_path,
                              "{\"button_tones\":\"false\",\"button_action\":\"bad\","
                              "\"button_action_sounds\":\"action-9\","
                              "\"button_action_brightness\":101,"
                              "\"button_mute_brightness\":-1}",
                              strlen("{\"button_tones\":\"false\",\"button_action\":\"bad\","
                                     "\"button_action_sounds\":\"action-9\","
                                     "\"button_action_brightness\":101,"
                                     "\"button_mute_brightness\":-1}")) == 0,
          "write corrupt persisted button fixture");
    CHECK(le_backend_init(&corrupt_backend, "mock", NULL, NULL, 9) == LE_OK &&
          api_init(&corrupt, corrupt_backend, 1, 1, NULL, NULL, csrf,
                   config_path, NULL) == 0,
          "initialise context with corrupt persisted settings");
    CHECK(corrupt.button_tones == 1 && !strcmp(corrupt.button_action, "sound") &&
          !strcmp(corrupt.button_action_sounds, "action-1,action-2,action-3") &&
          corrupt.button_action_brightness == 70 &&
          corrupt.button_mute_brightness == 60,
          "corrupt persisted settings fall back to defaults");

    le_backend_destroy(corrupt_backend);
    le_backend_destroy(restarted_backend);
    le_backend_destroy(backend);
    unlink(config_path);
    unlink("/tmp/libreecho-button-settings-test.json.bak");
    if (failures) {
        fprintf(stderr, "%d button settings test failures\n", failures);
        return 1;
    }
    puts("button settings: ok");
    return 0;
}
