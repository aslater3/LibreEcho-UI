/*
 * Discovery regression tests for btd.
 *
 * Two failures made a scan look broken from the web UI, both silently:
 *
 *   1. The device table is bounded at BT_MAX_DEVICES and nothing ever left it.
 *      LE peers advertise with rotating resolvable private addresses, so the
 *      table saturated within a scan or two, get_device() started returning
 *      NULL, and no newly discovered device could ever be reported again.
 *
 *   2. The status document is assembled into one bounded adapter message.  A
 *      full table of long EIR names overflowed it, status_json() returned -1,
 *      and btd answered with no reply at all -- the web API turned that into a
 *      503 that the Bluetooth page swallows to preserve its last good state,
 *      so the Nearby devices area simply stopped updating.
 *
 * The daemon source is compiled into this test so the real status writer and
 * the real discovery bookkeeping are exercised; the profile service layer is
 * stubbed out because it needs an L2CAP socket and the SBC codec.
 */
#define main btd_main
#include "adapter/btd.c"
#undef main

#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

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

/* A table full of the worst case the controller can hand us: every slot taken,
 * every entry both freshly discovered and bonded, and a 63-character EIR name
 * on each one. */
static void fill_table(struct bt_context *context, int bonded)
{
    size_t i;

    memset(context, 0, sizeof(*context));
    context->mgmt_fd = -1;
    context->enabled = 1;
    strcpy(context->local_name, "LibreEcho");
    for (i = 0; i < BT_MAX_DEVICES; ++i) {
        struct bt_device *device = &context->devices[context->device_count++];

        device->address[0] = (uint8_t)i;
        device->address[5] = 0xAA;
        device->type = 1;
        device->discovered = 1;
        device->paired = bonded;
        device->rssi = -101;
        device->rssi_valid = 1;
        snprintf(device->name, sizeof(device->name),
                 "%02u Long Bluetooth Friendly Name Carried In EIR Data!!",
                 (unsigned)i);
    }
}

static int status_reply(struct bt_context *context, char *response,
                        size_t response_size)
{
    char message[LE_ADAPTER_MSG_MAX];

    strcpy(message, "{\"v\":1,\"id\":4294967295,\"cmd\":\"status\",\"args\":{}}");
    return handle_request(context, message, response, response_size);
}

static int test_full_table_still_answers_status(void)
{
    struct bt_context context;
    char response[LE_ADAPTER_MSG_MAX];
    int length;

    fill_table(&context, 1);
    length = status_reply(&context, response, sizeof(response));
    CHECK(length > 0);
    CHECK((size_t)length < sizeof(response));
    CHECK(strstr(response, "\"ok\":true") != NULL);
    CHECK(strstr(response, "\"discovered\":[") != NULL);
    /* Bonds come first, so a crowded discovery list never starves the list the
     * user acts on. */
    CHECK(strstr(response, "\"known_devices\":[{\"address\"") != NULL);
    /* Clipped, not abandoned: the document must still close cleanly. */
    CHECK(strstr(response, "]}}\n") != NULL);
    return 0;
}

static int test_status_lists_every_device_when_it_fits(void)
{
    struct bt_context context;
    char response[LE_ADAPTER_MSG_MAX];
    size_t i, listed = 0;
    const char *at;

    fill_table(&context, 0);
    for (i = 0; i < context.device_count; ++i)
        snprintf(context.devices[i].name, sizeof(context.devices[i].name),
                 "Device %02u", (unsigned)i);
    CHECK(status_reply(&context, response, sizeof(response)) > 0);
    for (at = response; (at = strstr(at, "{\"address\"")) != NULL; ++at)
        ++listed;
    CHECK(listed == BT_MAX_DEVICES);
    return 0;
}

/* set_discovery() reaches the controller, which this host does not have.  A
 * socketpair whose peer is closed fails the management write immediately, so
 * the bookkeeping under test runs without a five-second command timeout. */
static int test_scan_start_frees_the_table(void)
{
    struct bt_context context;
    int pair[2];
    uint8_t address[6];

    fill_table(&context, 0);
    context.devices[0].paired = 1;
    context.devices[1].connected = 1;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    close(pair[1]);
    context.mgmt_fd = pair[0];
    signal(SIGPIPE, SIG_IGN);
    (void)set_discovery(&context, 1);
    close(pair[0]);
    context.mgmt_fd = -1;

    CHECK(context.device_count == 2);
    CHECK(context.devices[0].paired == 1);
    CHECK(context.devices[1].connected == 1);
    CHECK(context.devices[0].discovered == 0);
    /* The freed slots must accept the addresses the next scan reports. */
    memset(address, 0x5a, sizeof(address));
    CHECK(get_device(&context, address, 1) != NULL);
    return 0;
}

static int test_bond_name_clip_preserves_utf8(void)
{
    char name[64];
    char escaped[BT_STATUS_BOND_NAME_MAX * 2 + 8];

    memset(name, 'a', 29);
    memcpy(name + 29, "\xe2\x82\xac", 3); /* Euro sign: three UTF-8 bytes. */
    memcpy(name + 32, "tail", 5);
    bond_name_json(name, escaped, sizeof(escaped));
    CHECK(strstr(escaped, "\xe2\x82\xac...") != NULL);
    CHECK(strstr(escaped, "\xe2\x82\\x...") == NULL);
    return 0;
}

int main(void)
{
    if (test_full_table_still_answers_status() ||
        test_status_lists_every_device_when_it_fits() ||
        test_scan_start_frees_the_table() ||
        test_bond_name_clip_preserves_utf8())
        return 1;
    printf("bluetooth status capacity: ok\n");
    return 0;
}
