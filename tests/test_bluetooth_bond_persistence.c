/* Exercise real bond writes and event/status handling without hardware. */
#define DEVICE_DB "bluetooth.devices"
#define KEY_DB "bluetooth.keys"
#define le_adapter_connect test_adapter_connect
#define le_adapter_call test_adapter_call
#define le_adapter_close test_adapter_close
#define le_log test_log
#define main btd_main
#include "adapter/btd.c"
#undef main
#undef le_adapter_connect
#undef le_adapter_call
#undef le_adapter_close
#undef le_log

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static char led_args[192];
static char warning[512];
static int warning_count;
static int adapter_token;

struct le_adapter *test_adapter_connect(const char *path, int timeout)
{
    (void)path; (void)timeout;
    return (struct le_adapter *)&adapter_token;
}
void test_adapter_close(struct le_adapter *adapter) { (void)adapter; }
int test_adapter_call(struct le_adapter *adapter, const char *cmd,
                      const char *args, char *out, size_t size)
{
    (void)adapter; (void)cmd; (void)out; (void)size;
    snprintf(led_args, sizeof(led_args), "%s", args);
    return LE_ADAPTER_OK;
}
void test_log(enum le_log_level level, const char *fmt, ...)
{
    va_list args;
    size_t used = strlen(warning);
    if (level != LE_LOG_WARNING) return;
    va_start(args, fmt);
    vsnprintf(warning + used, sizeof(warning) - used, fmt, args);
    va_end(args);
    ++warning_count;
}

int le_profile_open(struct le_profiles *p, const char *name)
{
    (void)p; (void)name; return -1;
}
void le_profile_close(struct le_profiles *p) { (void)p; }
int le_profile_poll_setup(struct le_profiles *p, struct pollfd *pollfds,
                          int max_fds, int *fd_map)
{
    (void)p; (void)pollfds; (void)max_fds; (void)fd_map; return 0;
}
void le_profile_poll_events(struct le_profiles *p, const struct pollfd *pollfds,
                            const int *fd_map, int count)
{
    (void)p; (void)pollfds; (void)fd_map; (void)count;
}
int le_profile_registered_sdp(const struct le_profiles *p) { (void)p; return 0; }
int le_profile_registered_a2dp_sink(const struct le_profiles *p) { (void)p; return 0; }
int le_profile_registered_avrcp(const struct le_profiles *p) { (void)p; return 0; }
int le_profile_stream_active(const struct le_profiles *p) { (void)p; return 0; }

/* Directories reliably reject writes even when the test runs as root.
 * Test both fopen failure (.tmp is a directory) and rename failure (the
 * destination is a directory), separately and together, for both key types. */
static int test_event(uint16_t event, int failures, int rename_failure)
{
    struct bt_context context, restored;
    uint8_t payload[1 + sizeof(struct mgmt_ltk_wire)] = {0};
    char response[LE_ADAPTER_MSG_MAX];
    size_t size = 1 + (event == MGMT_EV_NEW_LINK_KEY ?
                      sizeof(struct mgmt_link_key_wire) : sizeof(struct mgmt_ltk_wire));
    const char *device_block = rename_failure ? DEVICE_DB : DEVICE_DB ".tmp";
    const char *key_block = rename_failure ? KEY_DB : KEY_DB ".tmp";
    const char *database = failures == 3 ? "device/key" : failures == 1 ? "device" : "key";
    char expected[96];

    memset(&context, 0, sizeof(context));
    context.mgmt_fd = -1;
    context.pairing.active = 1;
    payload[0] = 1;
    payload[1] = 0x42;
    payload[7] = event == MGMT_EV_NEW_LINK_KEY ? 0 : 1;
    warning[0] = led_args[0] = '\0';
    warning_count = 0;
    if (failures & 1) CHECK(mkdir(device_block, 0700) == 0);
    if (failures & 2) CHECK(mkdir(key_block, 0700) == 0);
    process_event(&context, event, payload, size);
    CHECK(context.device_count == 1 && context.devices[0].paired == 1);
    CHECK(context.link_key_count == (event == MGMT_EV_NEW_LINK_KEY ? 1U : 0U));
    CHECK(context.ltk_count == (event == MGMT_EV_NEW_LONG_TERM_KEY ? 1U : 0U));
    CHECK(context.pairing.active == 0);
    snprintf(expected, sizeof(expected),
             "Paired for this session only: %s database save failed: %s",
             database, strerror(EISDIR));
    CHECK(strcmp(context.last_error, expected) == 0);
    CHECK(warning_count == (failures == 3 ? 2 : 1));
    CHECK(strstr(warning, strerror(EISDIR)) != NULL);
    if (failures & 1) CHECK(strstr(warning, "device database save failed") != NULL);
    if (failures & 2) CHECK(strstr(warning, "key database save failed") != NULL);
    CHECK(strstr(led_args, "\"r\":255,\"g\":0,\"b\":0") != NULL);
    CHECK(status_json(&context, response, sizeof(response)) == 0);
    CHECK(strstr(response, expected) != NULL);
    CHECK(strstr(response, "\"known_devices\":[{\"address\"") != NULL);
    if (!(failures & 1)) CHECK(access(DEVICE_DB, F_OK) == 0);
    if (!(failures & 2)) CHECK(access(KEY_DB, F_OK) == 0);

    if (failures & 1) CHECK(rmdir(device_block) == 0);
    if (failures & 2) CHECK(rmdir(key_block) == 0);
    process_event(&context, event, payload, size);
    CHECK(context.last_error[0] == '\0');
    CHECK(warning_count == (failures == 3 ? 2 : 1));
    CHECK(strstr(led_args, "\"r\":0,\"g\":255,\"b\":0") != NULL);
    CHECK(status_json(&context, response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"last_error\":\"\"") != NULL);
    memset(&restored, 0, sizeof(restored));
    load_devices(&restored);
    load_keys(&restored);
    CHECK(restored.device_count == 1 && restored.devices[0].paired == 1);
    CHECK(restored.link_key_count == context.link_key_count);
    CHECK(restored.ltk_count == context.ltk_count);
    CHECK(memcmp(restored.link_keys, context.link_keys, sizeof(context.link_keys)) == 0);
    CHECK(memcmp(restored.ltks, context.ltks, sizeof(context.ltks)) == 0);
    CHECK(unlink(DEVICE_DB) == 0);
    CHECK(unlink(KEY_DB) == 0);
    return 0;
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    char directory[512], cwd[512];
    int failures, rename_failure, result = 0;
    if (!tmp || !tmp[0]) tmp = "build";
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
    if (tmp[0] == '/')
        CHECK(snprintf(directory, sizeof(directory), "%s/bt-bond-XXXXXX", tmp) < (int)sizeof(directory));
    else
        CHECK(snprintf(directory, sizeof(directory), "%s/%s/bt-bond-XXXXXX", cwd, tmp) < (int)sizeof(directory));
    CHECK(mkdtemp(directory) != NULL);
    CHECK(chdir(directory) == 0);
    for (rename_failure = 0; rename_failure <= 1 && !result; ++rename_failure)
        for (failures = 1; failures <= 3 && !result; ++failures)
            result = test_event(MGMT_EV_NEW_LINK_KEY, failures, rename_failure) ||
                     test_event(MGMT_EV_NEW_LONG_TERM_KEY, failures, rename_failure);
    if (result) {
        const char *paths[] = { DEVICE_DB, KEY_DB, DEVICE_DB ".tmp", KEY_DB ".tmp" };
        size_t i;
        for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
            (void)unlink(paths[i]);
            (void)rmdir(paths[i]);
        }
    }
    CHECK(chdir("/") == 0);
    CHECK(rmdir(directory) == 0);
    if (result) return 1;
    puts("bluetooth bond persistence: ok");
    return 0;
}
