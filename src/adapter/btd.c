/*
 * LibreEcho MT8163 Bluetooth companion daemon.
 *
 * The daemon owns the adapter boundary.  The kernel exposes the MT8163
 * controller as a native Linux HCI device; this process is the small
 * management-plane adapter used by the web service.  It deliberately keeps
 * pairing and discovery bounded and never retries a failed controller bring-up
 * in the same boot.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "adapter.h"
#include "log.h"

#include "bt_mgmt_events.h"
#include "bt_pairing_events.h"
#include "bt_profile.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SOCKET LE_ADAPTER_BLUETOOTH_SOCK
#define WMT_DEVICE "/dev/stpwmt"
#define BT_DEVICE "/dev/stpbt"
#define HCI_DEVICE "/sys/class/bluetooth/hci0"
#define BT_READY_PATH "/run/libreecho/bluetooth-ready"
#define CONFIGURE_HELPER "/sbin/wmt_configure"
#define BT_ON_HELPER "/sbin/wmt_bt_on"
#define FIRMWARE_DIR "/lib/firmware"
#define DEVICE_DB "/etc/libreecho/bluetooth.devices"
#define KEY_DB "/etc/libreecho/bluetooth.keys"
#define MEDIA_BUS_PATH "/run/libreecho-audio/media.pcm"

#define BTPROTO_HCI 1
#define BTPROTO_L2CAP 0
#define HCI_DEV_NONE 0xffff
#define HCI_DEV_ID 0
#define HCI_CHANNEL_RAW 0
#define HCI_CHANNEL_CONTROL 3
#define HCIGETCONNINFO _IOR('H', 213, int)
#define ACL_LINK 1
#define HCI_COMMAND_PKT 0x01
#define HCI_EVENT_PKT 0x04
#define EVT_CMD_COMPLETE 0x0e
#define HCI_OP_READ_RSSI 0x1405
#define SOL_HCI 0
#define HCI_FILTER 2
#define MGMT_PACKET_MAX 4096
#define MGMT_TIMEOUT_MS 5000
#define HELPER_TIMEOUT_TICKS 150
#define STARTUP_INFO_RETRY_COUNT 12
#define STARTUP_INFO_RETRY_DELAY_MS 500
#define BT_MAX_DEVICES 24
#define BT_MAX_KEYS 24
#define BT_NAME_MAX 64
/*
 * The status document travels inside the bounded adapter response envelope
 * ({"v":1,"id":N,"ok":true,"data":<document>}\n) and still has to close its own
 * arrays.  Clip the device lists against this reserve so a crowded table costs
 * a few listed devices instead of the whole snapshot: status_json() returning
 * -1 leaves the client with no reply at all, which the web UI can only render
 * as stale state.
 */
#define STATUS_ENVELOPE_RESERVE 64
#define BT_STATUS_BOND_NAME_MAX 32

/* L2CAP profile listeners.  PSM values follow the assigned-numbers table:
 * SDP browsing, AVRCP control/browsing, and AVDTP signaling/transport. */
#define LE_L2CAP_PSM_SDP 0x0001
#define LE_L2CAP_PSM_AVRCP 0x0017
#define LE_L2CAP_PSM_AVDTP 0x0019
#define LE_AVDTP_LOCAL_SEID 1U
#define LE_AVDTP_MEDIA_MTU 1024
#define LE_MAX_PROFILE_FDS 16

#define MGMT_STATUS_SUCCESS 0x00
#define MGMT_STATUS_FAILED 0x03
#define MGMT_STATUS_BUSY 0x0a
#define MGMT_STATUS_REJECTED 0x0b
#define MGMT_STATUS_NOT_SUPPORTED 0x0c
#define MGMT_STATUS_INVALID_PARAMS 0x0d
#define MGMT_EV_CMD_COMPLETE 0x0001
#define MGMT_EV_CMD_STATUS 0x0002
#define MGMT_EV_NEW_SETTINGS 0x0006
#define MGMT_EV_NEW_LINK_KEY 0x0009
#define MGMT_EV_NEW_LONG_TERM_KEY 0x000a
#define MGMT_EV_DEVICE_CONNECTED 0x000b
#define MGMT_EV_DEVICE_DISCONNECTED 0x000c
#define MGMT_EV_CONNECT_FAILED 0x000d
#define MGMT_EV_PIN_CODE_REQUEST 0x000e
#define MGMT_EV_USER_CONFIRM_REQUEST 0x000f
#define MGMT_EV_USER_PASSKEY_REQUEST 0x0010
#define MGMT_EV_AUTH_FAILED 0x0011
#define MGMT_EV_DEVICE_FOUND 0x0012
#define MGMT_EV_DISCOVERING 0x0013
#define MGMT_EV_DEVICE_UNPAIRED 0x0016
#define MGMT_EV_PASSKEY_NOTIFY 0x0017

#define MGMT_OP_READ_INFO 0x0004
#define MGMT_OP_SET_POWERED 0x0005
#define MGMT_OP_SET_SSP 0x000b
#define MGMT_OP_LOAD_LINK_KEYS 0x0012
#define MGMT_OP_LOAD_LONG_TERM_KEYS 0x0013
#define MGMT_OP_DISCONNECT 0x0014
#define MGMT_OP_PIN_CODE_REPLY 0x0016
#define MGMT_OP_PIN_CODE_NEG_REPLY 0x0017
#define MGMT_OP_SET_IO_CAPABILITY 0x0018
#define MGMT_OP_PAIR_DEVICE 0x0019
#define MGMT_OP_UNPAIR_DEVICE 0x001b
#define MGMT_OP_USER_CONFIRM_REPLY 0x001c
#define MGMT_OP_USER_CONFIRM_NEG_REPLY 0x001d
#define MGMT_OP_USER_PASSKEY_REPLY 0x001e
#define MGMT_OP_USER_PASSKEY_NEG_REPLY 0x001f
#define MGMT_OP_START_DISCOVERY 0x0023
#define MGMT_OP_STOP_DISCOVERY 0x0024
#define MGMT_OP_SET_DISCOVERABLE 0x0006
#define MGMT_OP_SET_CONNECTABLE 0x0007
#define MGMT_OP_SET_BONDABLE 0x0009
#define MGMT_OP_SET_LINK_SECURITY 0x000a
#define MGMT_OP_SET_DEV_CLASS 0x000e
#define MGMT_OP_SET_LOCAL_NAME 0x000f
#define MGMT_OP_ADD_UUID 0x0010

#define MGMT_SETTING_POWERED 0x00000001U
#define MGMT_SETTING_CONNECTABLE 0x00000002U
#define MGMT_SETTING_DISCOVERABLE 0x00000008U
#define MGMT_SETTING_BONDABLE 0x00000010U
#define MGMT_SETTING_LINK_SECURITY 0x00000020U
#define MGMT_SETTING_SSP 0x00000040U
#define MGMT_SETTING_BREDR 0x00000080U
#define MGMT_SETTING_LE 0x00000200U
#define MGMT_SETTING_SECURE_CONN 0x00000800U

#define MGMT_DISCOVERY_BREDR 0x01
#define MGMT_DISCOVERY_LE 0x06
#define MGMT_DISCOVERY_INTERLEAVED 0x07
#define MGMT_IO_CAP_DISPLAY_YES_NO 0x01
#define MGMT_IO_CAP_NO_INPUT_NO_OUTPUT 0x03
#define MGMT_MAX_NAME_LENGTH 249
#define MGMT_MAX_SHORT_NAME_LENGTH 11
#define MGMT_SET_LOCAL_NAME_SIZE \
    (MGMT_MAX_NAME_LENGTH + MGMT_MAX_SHORT_NAME_LENGTH)

/* Audio/Video major class, speaker/rendering minor class.  The management
 * API takes the already-positioned CoD major/minor bytes. */
#define LIBREECHO_BT_MAJOR_CLASS 0x04
#define LIBREECHO_BT_MINOR_CLASS 0x14
#define LIBREECHO_BT_NAME "LibreEcho"

/* Audio Sink (0x110B) in the Bluetooth base UUID byte order used by the
 * 3.18 management API.  0x24 advertises Audio + Rendering service classes. */
static const uint8_t libreecho_audio_sink_uuid[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x0b, 0x11, 0x00, 0x00,
};

struct sockaddr_hci {
    sa_family_t hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

struct hci_conn_info_local {
    uint16_t handle;
    uint8_t address[6];
    uint8_t type;
    uint8_t out;
    uint16_t state;
    uint32_t link_mode;
} __attribute__((packed));

struct hci_conn_info_req_local {
    uint8_t address[6];
    uint8_t type;
    uint8_t alignment_pad;
    struct hci_conn_info_local info;
} __attribute__((packed));

struct hci_filter_local {
    uint32_t type_mask;
    uint32_t event_mask[2];
    uint16_t opcode;
};

struct mgmt_hdr_wire {
    uint16_t opcode;
    uint16_t index;
    uint16_t length;
} __attribute__((packed));

struct mgmt_addr_wire {
    uint8_t address[6];
    uint8_t type;
} __attribute__((packed));

struct mgmt_link_key_wire {
    struct mgmt_addr_wire address;
    uint8_t type;
    uint8_t value[16];
    uint8_t pin_length;
} __attribute__((packed));

struct mgmt_ltk_wire {
    struct mgmt_addr_wire address;
    uint8_t type;
    uint8_t master;
    uint8_t encryption_size;
    uint16_t ediv;
    uint64_t random;
    uint8_t value[16];
} __attribute__((packed));

struct bt_device {
    uint8_t address[6];
    uint8_t type;
    char name[BT_NAME_MAX];
    int rssi;
    int rssi_valid;
    int discovered;
    int paired;
    int connected;
};

struct bt_link_key {
    struct mgmt_link_key_wire key;
};

struct bt_ltk {
    struct mgmt_ltk_wire key;
};

struct bt_pairing {
    int active;
    char method[24];
    uint8_t address[6];
    uint8_t type;
    unsigned int value;
};

struct bt_context {
    int activation_attempted;
    int enabled;
    int capability_ready;
    int mgmt_fd;
    int scanning;
    int pairing_mode;
    int pairing_saved_connectable;
    int pairing_saved_discoverable;
    int pairing_saved_bondable;
    int audio_sink_uuid_published;
    uint8_t discovery_type;
    unsigned int supported_settings;
    unsigned int current_settings;
    time_t last_info;
    char local_name[BT_NAME_MAX];
    char last_error[96];
    /* Issue #62 observability: most recent MGMT disconnect reason and
     * connect-failed status, rendered as "name (0xNN)".  Empty until the
     * first event of each kind has been observed. */
    char last_disconnect_reason[48];
    char last_connect_failed_status[48];
    struct bt_device devices[BT_MAX_DEVICES];
    size_t device_count;
    struct bt_link_key link_keys[BT_MAX_KEYS];
    size_t link_key_count;
    struct bt_ltk ltks[BT_MAX_KEYS];
    size_t ltk_count;
    struct bt_pairing pairing;
    struct le_profiles profiles;
    int profiles_opened;
};

static volatile sig_atomic_t stopping;

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void stop_daemon(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static const char *mgmt_status_name(unsigned int status)
{
    switch (status) {
    case 0x00: return "success";
    case 0x01: return "unknown-command";
    case 0x02: return "not-connected";
    case 0x03: return "failed";
    case 0x04: return "connect-failed";
    case 0x05: return "authentication-failed";
    case 0x06: return "not-paired";
    case 0x07: return "no-resources";
    case 0x08: return "timeout";
    case 0x09: return "already-connected";
    case 0x0a: return "busy";
    case 0x0b: return "rejected";
    case 0x0c: return "not-supported";
    case 0x0d: return "invalid-params";
    case 0x0e: return "disconnected";
    case 0x0f: return "not-powered";
    case 0x10: return "cancelled";
    case 0x11: return "invalid-index";
    case 0x12: return "rfkilled";
    case 0x13: return "already-paired";
    case 0x14: return "permission-denied";
    default: return "unknown-status";
    }
}

static void record_mgmt_status(struct bt_context *context, uint16_t opcode,
                               unsigned int status)
{
    if (!context)
        return;
    snprintf(context->last_error, sizeof(context->last_error),
             "MGMT opcode 0x%04x status 0x%02x (%s)", opcode, status,
             mgmt_status_name(status));
}

static void record_mgmt_io(struct bt_context *context, uint16_t opcode,
                           const char *phase, int error_number)
{
    if (!context)
        return;
    snprintf(context->last_error, sizeof(context->last_error),
             "MGMT opcode 0x%04x %s failed: %s", opcode, phase,
             strerror(error_number ? error_number : EIO));
}

static int write_all(int fd, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < length) {
        ssize_t written = write(fd, bytes + sent, length - sent);
        if (written > 0) {
            sent += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int mgmt_write_all(int fd, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < length) {
        struct pollfd pollfd = { .fd = fd, .events = POLLOUT };
        ssize_t written = write(fd, bytes + sent, length - sent);
        if (written > 0) {
            sent += (size_t)written;
            continue;
        }
        if (written < 0 && (errno == EINTR || errno == EAGAIN ||
                            errno == EWOULDBLOCK)) {
            if (poll(&pollfd, 1, MGMT_TIMEOUT_MS) <= 0)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int read_line(int fd, char *buffer, size_t size)
{
    size_t used = 0;

    if (!buffer || size < 2)
        return -1;
    while (used + 1 < size) {
        struct pollfd pollfd = { .fd = fd, .events = POLLIN };
        char character;
        ssize_t count;

        do {
            count = poll(&pollfd, 1, 5000);
        } while (count < 0 && errno == EINTR);
        if (count <= 0 || !(pollfd.revents & POLLIN))
            return -1;
        do {
            count = read(fd, &character, 1);
        } while (count < 0 && errno == EINTR);
        if (count != 1)
            return -1;
        if (character == '\n') {
            buffer[used] = '\0';
            return 0;
        }
        buffer[used++] = character;
    }
    return -1;
}

static int run_helper(const char *path, const char *name,
                      const char *const argv[])
{
    pid_t child;
    int status;
    int tick;

    child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        execv(path, (char *const *)argv);
        _exit(127);
    }
    for (tick = 0; tick < HELPER_TIMEOUT_TICKS; ++tick) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000L };

        if (waited == child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                le_log_info("btd: %s completed", name);
                return 0;
            }
            le_log_warn("btd: %s failed (status=%d)", name, status);
            return -1;
        }
        if (waited < 0 && errno != EINTR)
            return -1;
        (void)nanosleep(&delay, NULL);
    }
    (void)kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
    le_log_warn("btd: %s timed out", name);
    return -1;
}

static int wmt_present(void)
{
    return access(WMT_DEVICE, F_OK) == 0;
}

static int hci_present(void)
{
    return access(HCI_DEVICE, F_OK) == 0;
}

static void set_controller_ready(int ready)
{
    int fd;

    if (!ready) {
        unlink(BT_READY_PATH);
        return;
    }
    fd = open(BT_READY_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        le_log_warn("btd: could not publish controller readiness: %s",
                    strerror(errno));
        return;
    }
    if (write(fd, "ready\n", 6) != 6)
        le_log_warn("btd: could not write controller readiness: %s",
                    strerror(errno));
    close(fd);
}

static void address_text(const uint8_t *address, char *text, size_t size)
{
    if (!address || !text || size < 18)
        return;
    snprintf(text, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[5], address[4], address[3], address[2], address[1],
             address[0]);
}

static int parse_address(const char *text, uint8_t *address)
{
    unsigned int b[6];
    int i;

    if (!text || !address ||
        sscanf(text, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) != 6)
        return -1;
    for (i = 0; i < 6; ++i)
        if (b[i] > 255)
            return -1;
    for (i = 0; i < 6; ++i)
        address[i] = (uint8_t)b[i];
    return 0;
}

static int same_address(const uint8_t *a, const uint8_t *b)
{
    return a && b && memcmp(a, b, 6) == 0;
}

static struct bt_device *find_device(struct bt_context *context,
                                     const uint8_t *address, uint8_t type)
{
    size_t i;

    for (i = 0; i < context->device_count; ++i)
        if (same_address(context->devices[i].address, address) &&
            context->devices[i].type == type)
            return &context->devices[i];
    return NULL;
}

static struct bt_device *get_device(struct bt_context *context,
                                    const uint8_t *address, uint8_t type)
{
    struct bt_device *device = find_device(context, address, type);

    if (device)
        return device;
    if (context->device_count >= BT_MAX_DEVICES)
        return NULL;
    device = &context->devices[context->device_count++];
    memset(device, 0, sizeof(*device));
    memcpy(device->address, address, 6);
    device->type = type;
    strcpy(device->name, "Unknown device");
    return device;
}

static void json_escape(const char *input, char *output, size_t size)
{
    size_t used = 0;

    if (!output || !size)
        return;
    if (!input)
        input = "";
    while (*input && used + 2 < size) {
        unsigned char c = (unsigned char)*input++;
        if (c == '"' || c == '\\') {
            output[used++] = '\\';
            output[used++] = (char)c;
        } else if (c >= 0x20) {
            output[used++] = (char)c;
        }
    }
    output[used] = '\0';
}

static size_t utf8_prefix(const char *input, size_t max)
{
    size_t used = 0;

    while (input[used] && used < max) {
        unsigned char first = (unsigned char)input[used];
        size_t width = 1;
        size_t i;

        if (first >= 0xc2 && first <= 0xdf)
            width = 2;
        else if (first >= 0xe0 && first <= 0xef)
            width = 3;
        else if (first >= 0xf0 && first <= 0xf4)
            width = 4;
        if (width > 1) {
            if (used + width > max)
                break;
            for (i = 1; i < width; ++i)
                if (((unsigned char)input[used + i] & 0xc0) != 0x80)
                    width = 1;
        }
        if (used + width > max)
            break;
        used += width;
    }
    return used;
}

static void bond_name_json(const char *input, char *output, size_t size)
{
    char clipped[BT_STATUS_BOND_NAME_MAX + 4];
    size_t raw_length = strlen(input);
    size_t prefix = utf8_prefix(input, BT_STATUS_BOND_NAME_MAX);

    if (raw_length > prefix && prefix + 3 < sizeof(clipped)) {
        memcpy(clipped, input, prefix);
        memcpy(clipped + prefix, "...", 4);
    } else {
        prefix = raw_length < sizeof(clipped) - 1 ? raw_length :
                 sizeof(clipped) - 1;
        memcpy(clipped, input, prefix);
        clipped[prefix] = '\0';
    }
    json_escape(clipped, output, size);
}

static int append_text(char *buffer, size_t size, size_t *used,
                       const char *format, ...)
{
    va_list ap;
    int written;

    if (!buffer || !used || *used >= size)
        return -1;
    va_start(ap, format);
    written = vsnprintf(buffer + *used, size - *used, format, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= size - *used)
        return -1;
    *used += (size_t)written;
    return 0;
}

static const char *json_value(const char *json, const char *key)
{
    char needle[64];
    const char *p;

    if (!json || !key || snprintf(needle, sizeof(needle), "\"%s\"", key) < 0)
        return NULL;
    p = strstr(json, needle);
    if (!p)
        return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (*p++ != ':')
        return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    return p;
}

static int json_string(const char *json, const char *key, char *output,
                       size_t size)
{
    const char *p = json_value(json, key);
    size_t used = 0;

    if (!p || *p++ != '"' || !output || size < 2)
        return -1;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p)
            c = *p++;
        if (used + 1 >= size)
            return -1;
        output[used++] = c;
    }
    if (*p != '"')
        return -1;
    output[used] = '\0';
    return 0;
}

static int json_integer(const char *json, const char *key, int *value)
{
    const char *p = json_value(json, key);
    char *end;
    long result;

    if (!p || !value)
        return -1;
    result = strtol(p, &end, 10);
    if (end == p || result < -2147483647L || result > 2147483647L)
        return -1;
    *value = (int)result;
    return 0;
}

static int save_devices(const struct bt_context *context)
{
    char temporary[sizeof(DEVICE_DB) + 8];
    FILE *file;
    size_t i;

    (void)snprintf(temporary, sizeof(temporary), "%s.tmp", DEVICE_DB);
    file = fopen(temporary, "w");
    if (!file)
        return -1;
    (void)fchmod(fileno(file), 0600);
    for (i = 0; i < context->device_count; ++i) {
        char address[18];
        if (!context->devices[i].paired)
            continue;
        address_text(context->devices[i].address, address, sizeof(address));
        if (fprintf(file, "%s %u %d %d %s\n", address,
                    context->devices[i].type, context->devices[i].rssi_valid,
                    context->devices[i].rssi, context->devices[i].name) < 0) {
            fclose(file);
            unlink(temporary);
            return -1;
        }
    }
    if (fclose(file) != 0 || rename(temporary, DEVICE_DB) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static void load_devices(struct bt_context *context)
{
    FILE *file = fopen(DEVICE_DB, "r");
    char line[160];

    if (!file)
        return;
    while (fgets(line, sizeof(line), file)) {
        char address_text_value[18], name[BT_NAME_MAX];
        unsigned int type;
        int rssi_valid = 0;
        int rssi = 0;
        uint8_t address[6];
        struct bt_device *device;

        name[0] = '\0';
        if (sscanf(line, "%17s %u %d %d %63[^\n]", address_text_value,
                   &type, &rssi_valid, &rssi, name) != 5) {
            if (sscanf(line, "%17s %u %63[^\n]", address_text_value,
                       &type, name) < 2)
                continue;
        }
        if (type > 255 || parse_address(address_text_value, address) != 0)
            continue;
        device = get_device(context, address, (uint8_t)type);
        if (!device)
            break;
        device->paired = 1;
        device->rssi_valid = rssi_valid != 0;
        device->rssi = rssi;
        if (name[0]) {
            strncpy(device->name, name, sizeof(device->name) - 1);
            device->name[sizeof(device->name) - 1] = '\0';
        }
    }
    fclose(file);
}

struct key_file_header {
    uint32_t magic;
    uint16_t version;
    uint16_t link_count;
    uint16_t ltk_count;
} __attribute__((packed));

#define KEY_FILE_MAGIC 0x4c454254U
#define KEY_FILE_VERSION 1

static int save_keys(const struct bt_context *context)
{
    char temporary[sizeof(KEY_DB) + 8];
    struct key_file_header header;
    FILE *file;

    (void)snprintf(temporary, sizeof(temporary), "%s.tmp", KEY_DB);
    file = fopen(temporary, "wb");
    if (!file)
        return -1;
    (void)fchmod(fileno(file), 0600);
    header.magic = KEY_FILE_MAGIC;
    header.version = KEY_FILE_VERSION;
    header.link_count = (uint16_t)context->link_key_count;
    header.ltk_count = (uint16_t)context->ltk_count;
    if (fwrite(&header, sizeof(header), 1, file) != 1 ||
        (context->link_key_count &&
         fwrite(context->link_keys, sizeof(context->link_keys[0]),
                context->link_key_count, file) != context->link_key_count) ||
        (context->ltk_count &&
         fwrite(context->ltks, sizeof(context->ltks[0]), context->ltk_count,
                file) != context->ltk_count)) {
        fclose(file);
        unlink(temporary);
        return -1;
    }
    if (fclose(file) != 0 || rename(temporary, KEY_DB) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static void load_keys(struct bt_context *context)
{
    struct key_file_header header;
    FILE *file = fopen(KEY_DB, "rb");

    if (!file)
        return;
    if (fread(&header, sizeof(header), 1, file) != 1 ||
        header.magic != KEY_FILE_MAGIC || header.version != KEY_FILE_VERSION ||
        header.link_count > BT_MAX_KEYS || header.ltk_count > BT_MAX_KEYS ||
        fread(context->link_keys, sizeof(context->link_keys[0]),
              header.link_count, file) != header.link_count ||
        fread(context->ltks, sizeof(context->ltks[0]), header.ltk_count, file) !=
            header.ltk_count) {
        fclose(file);
        memset(context->link_keys, 0, sizeof(context->link_keys));
        memset(context->ltks, 0, sizeof(context->ltks));
        return;
    }
    context->link_key_count = header.link_count;
    context->ltk_count = header.ltk_count;
    fclose(file);
}

static int mgmt_open(struct bt_context *context)
{
    struct sockaddr_hci address;
    int flags;

    if (context->mgmt_fd >= 0)
        return 0;
    context->mgmt_fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (context->mgmt_fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.hci_family = AF_BLUETOOTH;
    address.hci_dev = HCI_DEV_NONE;
    address.hci_channel = HCI_CHANNEL_CONTROL;
    if (bind(context->mgmt_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(context->mgmt_fd);
        context->mgmt_fd = -1;
        return -1;
    }
    flags = fcntl(context->mgmt_fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(context->mgmt_fd, F_SETFL, flags | O_NONBLOCK);
    return 0;
}

static void led_pattern(const char *name, unsigned int r, unsigned int g,
                        unsigned int b, unsigned int brightness,
                        unsigned int repeats)
{
    struct le_adapter *adapter;
    char args[192], response[128];
    int result;

    adapter = le_adapter_connect(LE_ADAPTER_LED_SOCK, 100);
    if (!adapter) {
        le_log_debug("btd: LED pattern daemon unavailable for %s", name);
        return;
    }
    snprintf(args, sizeof(args),
             "{\"name\":\"%s\",\"r\":%u,\"g\":%u,\"b\":%u,"
             "\"brightness\":%u,\"repeats\":%u,\"owner\":\"bluetooth\"}",
             name, r, g, b, brightness, repeats);
    result = le_adapter_call(adapter, "pattern", args, response,
                             sizeof(response));
    if (result != LE_ADAPTER_OK)
        le_log_warn("btd: LED pattern %s failed (%d)", name, result);
    le_adapter_close(adapter);
}

static void pairing_clear(struct bt_context *context)
{
    memset(&context->pairing, 0, sizeof(context->pairing));
}

static void update_pairing(struct bt_context *context, const uint8_t *payload,
                           size_t size, const char *method, unsigned int value)
{
    char address[18];

    if (size < sizeof(struct mgmt_addr_wire))
        return;
    memcpy(context->pairing.address, payload, 6);
    context->pairing.type = payload[6];
    context->pairing.active = 1;
    strncpy(context->pairing.method, method,
            sizeof(context->pairing.method) - 1);
    context->pairing.value = value;
    /* Issue #62: log every inbound pairing/authentication request so the
     * pairing lifecycle is visible from the daemon without a raw monitor. */
    address_text(payload, address, sizeof(address));
    le_log_info("btd: MGMT pairing request: address=%s type=%u method=%s "
                "value=%u",
                address, payload[6], method, value);
}

static void eir_name(const uint8_t *eir, size_t size, char *name, size_t name_size)
{
    size_t offset = 0;

    while (offset < size) {
        size_t length = eir[offset++];
        uint8_t type;
        size_t copy;

        if (!length || offset + length > size)
            break;
        type = eir[offset];
        if (type == 0x08 || type == 0x09) {
            copy = length - 1;
            if (copy >= name_size)
                copy = name_size - 1;
            memcpy(name, eir + offset + 1, copy);
            name[copy] = '\0';
            return;
        }
        offset += length;
    }
}

static int update_device_from_eir(struct bt_device *device,
                                  const uint8_t *eir, size_t size)
{
    char name[BT_NAME_MAX];

    if (!device || !eir)
        return 0;
    name[0] = '\0';
    eir_name(eir, size, name, sizeof(name));
    if (!name[0])
        return 0;
    if (!strcmp(device->name, name))
        return 0;
    strncpy(device->name, name, sizeof(device->name) - 1);
    device->name[sizeof(device->name) - 1] = '\0';
    return 1;
}

static int read_remote_rssi(const uint8_t *address, uint8_t type, int *rssi)
{
    struct hci_conn_info_req_local request;
    struct hci_filter_local filter;
    struct sockaddr_hci raw_address;
    struct pollfd pollfd;
    uint8_t command[6];
    uint8_t event[64];
    int fd;
    ssize_t count;
    int waited = 0;
    int attempts = 0;

    if (!address || !rssi || type != 0)
        return -1;
    fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0)
        return -1;
    memset(&raw_address, 0, sizeof(raw_address));
    raw_address.hci_family = AF_BLUETOOTH;
    raw_address.hci_dev = HCI_DEV_ID;
    raw_address.hci_channel = HCI_CHANNEL_RAW;
    memset(&request, 0, sizeof(request));
    memcpy(request.address, address, sizeof(request.address));
    request.type = ACL_LINK;
    if (bind(fd, (struct sockaddr *)&raw_address, sizeof(raw_address)) < 0) {
        close(fd);
        return -1;
    }
    memset(&filter, 0, sizeof(filter));
    filter.type_mask = 1U << HCI_EVENT_PKT;
    filter.event_mask[0] = 1U << EVT_CMD_COMPLETE;
    filter.opcode = HCI_OP_READ_RSSI;
    if (setsockopt(fd, SOL_HCI, HCI_FILTER, &filter, sizeof(filter)) < 0) {
        close(fd);
        return -1;
    }
    while (ioctl(fd, HCIGETCONNINFO, &request) < 0 && attempts++ < 10) {
        struct timespec delay = { 0, 100000000L };
        nanosleep(&delay, NULL);
    }
    if (attempts > 10) {
        close(fd);
        return -1;
    }
    command[0] = HCI_COMMAND_PKT;
    command[1] = (uint8_t)HCI_OP_READ_RSSI;
    command[2] = (uint8_t)(HCI_OP_READ_RSSI >> 8);
    command[3] = 2;
    command[4] = (uint8_t)request.info.handle;
    command[5] = (uint8_t)(request.info.handle >> 8);
    if (write(fd, command, sizeof(command)) != (ssize_t)sizeof(command)) {
        close(fd);
        return -1;
    }
    while (waited < 1000) {
        pollfd.fd = fd;
        pollfd.events = POLLIN;
        if (poll(&pollfd, 1, 100) <= 0) {
            waited += 100;
            continue;
        }
        count = read(fd, event, sizeof(event));
        if (count >= 10 && event[0] == HCI_EVENT_PKT &&
            event[1] == EVT_CMD_COMPLETE &&
            event[4] == (uint8_t)HCI_OP_READ_RSSI &&
            event[5] == (uint8_t)(HCI_OP_READ_RSSI >> 8) &&
            event[6] == 0 &&
            event[7] == command[4] && event[8] == command[5]) {
            *rssi = (int8_t)event[9];
            close(fd);
            return 0;
        }
    }
    close(fd);
    return -1;
}

static void refresh_connected_rssi(struct bt_context *context)
{
    size_t i;

    for (i = 0; i < context->device_count; ++i) {
        struct bt_device *device = &context->devices[i];
        int rssi;

        if (!device->connected ||
            read_remote_rssi(device->address, device->type, &rssi) != 0)
            continue;
        device->rssi = rssi;
        device->rssi_valid = 1;
        if (device->paired)
            (void)save_devices(context);
    }
}

static void process_event(struct bt_context *context, uint16_t event,
                           const uint8_t *payload, size_t size)
{
    struct bt_device *device;

    switch (event) {
    case MGMT_EV_NEW_SETTINGS:
        if (size >= 4)
            context->current_settings = read_le32(payload);
        return;
    case MGMT_EV_DISCOVERING:
        if (size >= 2) {
            context->discovery_type = payload[0];
            context->scanning = payload[1] != 0;
            if (!context->scanning)
                context->discovery_type = 0;
        }
        return;
    case MGMT_EV_DEVICE_FOUND:
        if (size >= 14) {
            char name[BT_NAME_MAX];
            int rssi = (int8_t)payload[7];
            size_t eir_size = read_le16(payload + 12);
            name[0] = '\0';
            if (eir_size > size - 14)
                eir_size = size - 14;
            eir_name(payload + 14, eir_size, name, sizeof(name));
            device = get_device(context, payload, payload[6]);
            if (!device)
                return;
            device->discovered = 1;
            device->rssi = rssi;
            device->rssi_valid = 1;
            if (name[0]) {
                strncpy(device->name, name, sizeof(device->name) - 1);
                device->name[sizeof(device->name) - 1] = '\0';
            }
            if (device->paired)
                (void)save_devices(context);
        }
        return;
    case MGMT_EV_DEVICE_CONNECTED:
        if (size >= 7) {
            char address[18];
            int metadata_changed = 0;

            device = get_device(context, payload, payload[6]);
            if (device) {
                device->connected = 1;
                if (size >= 13) {
                    size_t eir_size = read_le16(payload + 11);
                    if (eir_size > size - 13)
                        eir_size = size - 13;
                    metadata_changed = update_device_from_eir(
                        device, payload + 13, eir_size);
                }
                if (read_remote_rssi(device->address, device->type,
                                     &device->rssi) == 0) {
                    device->rssi_valid = 1;
                    metadata_changed = 1;
                }
                if (metadata_changed && device->paired)
                    (void)save_devices(context);
            }
            address_text(payload, address, sizeof(address));
            le_log_info("btd: MGMT device connected: address=%s type=%u",
                        address, payload[6]);
        }
        return;
    case MGMT_EV_DEVICE_DISCONNECTED: {
        char reason_text[48];
        char address[18];

        if (size >= 7) {
            device = find_device(context, payload, payload[6]);
            if (device)
                device->connected = 0;
        }
        /* Issue #62: retain and log the disconnect reason so pairing and
         * connection triage is possible from the daemon alone. */
        if (le_bt_mgmt_disconnect_reason_text(payload, size, reason_text,
                                              sizeof(reason_text)) == 0) {
            strncpy(context->last_disconnect_reason, reason_text,
                    sizeof(context->last_disconnect_reason) - 1);
            context->last_disconnect_reason[
                sizeof(context->last_disconnect_reason) - 1] = '\0';
        }
        if (size >= 7) {
            address_text(payload, address, sizeof(address));
            le_log_info("btd: MGMT device disconnected: address=%s type=%u "
                        "reason=%s",
                        address, payload[6],
                        context->last_disconnect_reason[0] ?
                        context->last_disconnect_reason : "unknown");
        } else {
            le_log_info("btd: MGMT device disconnected: truncated event "
                        "(size=%zu)", size);
        }
        return;
    }
    case MGMT_EV_CONNECT_FAILED: {
        char status_text[48];
        char address[18];

        if (size >= 7) {
            device = find_device(context, payload, payload[6]);
            if (device)
                device->connected = 0;
        }
        /* Issue #62: retain the connect-failed status in last_error and a
         * dedicated status field; keep the pairing LED feedback unchanged. */
        if (le_bt_mgmt_connect_failed_text(payload, size, status_text,
                                           sizeof(status_text)) == 0) {
            strncpy(context->last_connect_failed_status, status_text,
                    sizeof(context->last_connect_failed_status) - 1);
            context->last_connect_failed_status[
                sizeof(context->last_connect_failed_status) - 1] = '\0';
            if (size >= 7) {
                char status_short[40];

                address_text(payload, address, sizeof(address));
                /* Bound the status text so the rendered message cannot exceed
                 * last_error: address (17) + status (<=39) + decoration fits. */
                strncpy(status_short, status_text, sizeof(status_short) - 1);
                status_short[sizeof(status_short) - 1] = '\0';
                snprintf(context->last_error, sizeof(context->last_error),
                         "Bluetooth connection failed: %s (%s)",
                         address, status_short);
                le_log_warn("btd: MGMT connect failed: address=%s type=%u "
                            "status=%s",
                            address, payload[6], status_text);
            } else {
                le_log_warn("btd: MGMT connect failed: truncated event "
                            "(size=%zu)", size);
            }
        } else {
            le_log_warn("btd: MGMT connect failed: truncated event (size=%zu)",
                        size);
        }
        if (context->pairing.active)
            led_pattern("flash", 255, 0, 0, 100, 1);
        return;
    }
    case MGMT_EV_DEVICE_UNPAIRED:
        if (size >= 7) {
            device = find_device(context, payload, payload[6]);
            if (device)
                device->paired = 0;
            if (context->pairing.active && same_address(context->pairing.address,
                                                        payload))
                pairing_clear(context);
        }
        return;
    case MGMT_EV_PIN_CODE_REQUEST:
        update_pairing(context, payload, size, "pin", 0);
        return;
    case MGMT_EV_USER_CONFIRM_REQUEST: {
        uint32_t value;

        if (le_bt_pairing_event_value(event, payload, size, &value) == 0)
            update_pairing(context, payload, size, "confirm", value);
        return;
    }
    case MGMT_EV_USER_PASSKEY_REQUEST:
        update_pairing(context, payload, size, "passkey", 0);
        return;
    case MGMT_EV_PASSKEY_NOTIFY: {
        uint32_t value;

        if (le_bt_pairing_event_value(event, payload, size, &value) == 0)
            update_pairing(context, payload, size, "notify", value);
        return;
    }
    case MGMT_EV_AUTH_FAILED: {
        char address[18];

        if (size >= 7) {
            /* Issue #62: log authentication failures with the remote address
             * so pairing triage is possible from the daemon alone. */
            address_text(payload, address, sizeof(address));
            le_log_warn("btd: MGMT authentication failed: address=%s type=%u",
                        address, payload[6]);
        }
        if (size >= 7 && context->pairing.active &&
            same_address(context->pairing.address, payload))
            snprintf(context->last_error, sizeof(context->last_error),
                     "Bluetooth authentication failed");
        if (context->pairing.active)
            led_pattern("flash", 255, 0, 0, 100, 1);
        pairing_clear(context);
        return;
    }
    case MGMT_EV_NEW_LINK_KEY:
        if (size >= 1 + sizeof(struct mgmt_link_key_wire)) {
            const struct mgmt_link_key_wire *key =
                (const struct mgmt_link_key_wire *)(payload + 1);
            struct bt_device *saved = get_device(context, key->address.address,
                                                 key->address.type);
            size_t i;
            if (saved)
                saved->paired = 1;
            for (i = 0; i < context->link_key_count; ++i)
                if (memcmp(&context->link_keys[i].key.address, &key->address,
                           sizeof(key->address)) == 0)
                    break;
            if (i == context->link_key_count && i < BT_MAX_KEYS)
                context->link_key_count++;
            if (i < BT_MAX_KEYS)
                memcpy(&context->link_keys[i].key, key, sizeof(*key));
            (void)save_devices(context);
            (void)save_keys(context);
            led_pattern("flash", 0, 255, 0, 100, 3);
            pairing_clear(context);
        }
        return;
    case MGMT_EV_NEW_LONG_TERM_KEY:
        if (size >= 1 + sizeof(struct mgmt_ltk_wire)) {
            const struct mgmt_ltk_wire *key =
                (const struct mgmt_ltk_wire *)(payload + 1);
            struct bt_device *saved = get_device(context, key->address.address,
                                                 key->address.type);
            size_t i;
            if (saved)
                saved->paired = 1;
            for (i = 0; i < context->ltk_count; ++i)
                if (memcmp(&context->ltks[i].key.address, &key->address,
                           sizeof(key->address)) == 0)
                    break;
            if (i == context->ltk_count && i < BT_MAX_KEYS)
                context->ltk_count++;
            if (i < BT_MAX_KEYS)
                memcpy(&context->ltks[i].key, key, sizeof(*key));
            (void)save_devices(context);
            (void)save_keys(context);
            led_pattern("flash", 0, 255, 0, 100, 3);
            pairing_clear(context);
        }
        return;
    default:
        return;
    }
}

static int process_packet(struct bt_context *context, const uint8_t *packet,
                          size_t size, uint16_t expected, uint8_t *reply,
                          size_t *reply_size)
{
    uint16_t event;
    uint16_t length;
    const uint8_t *payload;

    if (size < sizeof(struct mgmt_hdr_wire))
        return 0;
    event = read_le16(packet);
    length = read_le16(packet + 4);
    if ((size_t)length + sizeof(struct mgmt_hdr_wire) > size)
        return 0;
    payload = packet + sizeof(struct mgmt_hdr_wire);
    process_event(context, event, payload, length);
    if (event == MGMT_EV_CMD_COMPLETE && length >= 3 &&
        read_le16(payload) == expected) {
        if (payload[2] != MGMT_STATUS_SUCCESS) {
            record_mgmt_status(context, expected, payload[2]);
            le_log_warn("btd: management opcode 0x%04x failed (status=0x%02x, %s)",
                        expected, payload[2], mgmt_status_name(payload[2]));
            return -(int)payload[2];
        }
        if (reply && reply_size) {
            size_t data_size = length - 3;
            if (data_size > *reply_size)
                return -1;
            memcpy(reply, payload + 3, data_size);
            *reply_size = data_size;
        }
        return 1;
    }
    if (event == MGMT_EV_CMD_STATUS && length >= 3 &&
        read_le16(payload) == expected) {
        if (payload[2] != MGMT_STATUS_SUCCESS) {
            record_mgmt_status(context, expected, payload[2]);
            le_log_warn("btd: management opcode 0x%04x rejected (status=0x%02x, %s)",
                        expected, payload[2], mgmt_status_name(payload[2]));
            return -(int)payload[2];
        }
        return 1;
    }
    return 0;
}

static int mgmt_command(struct bt_context *context, uint16_t opcode,
                        const void *payload, size_t payload_size,
                        uint8_t *reply, size_t *reply_size)
{
    uint8_t packet[MGMT_PACKET_MAX];
    struct mgmt_hdr_wire header;
    struct pollfd pollfd;
    size_t packet_size = sizeof(header) + payload_size;
    int waited = 0;
    int saved_errno;

    if (packet_size > sizeof(packet)) {
        record_mgmt_io(context, opcode, "packet", EMSGSIZE);
        errno = EMSGSIZE;
        return -1;
    }
    if (mgmt_open(context) != 0) {
        saved_errno = errno;
        record_mgmt_io(context, opcode, "open", saved_errno);
        errno = saved_errno;
        return -1;
    }
    header.opcode = opcode;
    header.index = 0;
    header.length = (uint16_t)payload_size;
    memcpy(packet, &header, sizeof(header));
    if (payload_size)
        memcpy(packet + sizeof(header), payload, payload_size);
    if (mgmt_write_all(context->mgmt_fd, packet, packet_size) != 0) {
        saved_errno = errno;
        record_mgmt_io(context, opcode, "write", saved_errno);
        errno = saved_errno;
        return -1;
    }
    while (waited < MGMT_TIMEOUT_MS) {
        uint8_t response[MGMT_PACKET_MAX];
        ssize_t count;
        int result;

        pollfd.fd = context->mgmt_fd;
        pollfd.events = POLLIN;
        {
            int poll_result = poll(&pollfd, 1, 250);

            if (poll_result == 0) {
                waited += 250;
                continue;
            }
            if (poll_result < 0) {
                if (errno == EINTR)
                    continue;
                saved_errno = errno;
                record_mgmt_io(context, opcode, "poll", saved_errno);
                errno = saved_errno;
                return -1;
            }
        }
        count = read(context->mgmt_fd, response, sizeof(response));
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            waited += 250;
            continue;
        }
        if (count <= 0) {
            record_mgmt_io(context, opcode, "read", count == 0 ? ECONNRESET : errno);
            if (count == 0)
                errno = ECONNRESET;
            return -1;
        }
        result = process_packet(context, response, (size_t)count, opcode,
                                reply, reply_size);
        if (result != 0)
            return result > 0 ? 0 : result;
        waited += 250;
    }
    record_mgmt_io(context, opcode, "response", ETIMEDOUT);
    errno = ETIMEDOUT;
    return -1;
}

static int load_keys_into_controller(struct bt_context *context)
{
    uint8_t payload[3 + BT_MAX_KEYS * sizeof(struct mgmt_link_key_wire)];
    uint8_t ltk_payload[2 + BT_MAX_KEYS * sizeof(struct mgmt_ltk_wire)];
    size_t size;

    if (context->link_key_count) {
        memset(payload, 0, sizeof(payload));
        write_le16(payload + 1, (uint16_t)context->link_key_count);
        memcpy(payload + 3, context->link_keys,
               context->link_key_count * sizeof(context->link_keys[0]));
        size = 3 + context->link_key_count * sizeof(struct mgmt_link_key_wire);
        if (mgmt_command(context, MGMT_OP_LOAD_LINK_KEYS, payload, size, NULL,
                         NULL) != 0)
            return -1;
    }
    if (context->ltk_count) {
        memset(ltk_payload, 0, sizeof(ltk_payload));
        write_le16(ltk_payload, (uint16_t)context->ltk_count);
        memcpy(ltk_payload + 2, context->ltks,
               context->ltk_count * sizeof(context->ltks[0]));
        size = 2 + context->ltk_count * sizeof(struct mgmt_ltk_wire);
        if (mgmt_command(context, MGMT_OP_LOAD_LONG_TERM_KEYS, ltk_payload, size,
                         NULL, NULL) != 0)
            return -1;
    }
    return 0;
}

static int refresh_info(struct bt_context *context)
{
    uint8_t reply[512];
    size_t reply_size = sizeof(reply);

    if (!hci_present() || mgmt_command(context, MGMT_OP_READ_INFO, NULL, 0,
                                       reply, &reply_size) != 0) {
        set_controller_ready(0);
        return -1;
    }
    if (reply_size >= 20) {
        context->supported_settings = read_le32(reply + 9);
        context->current_settings = read_le32(reply + 13);
        if (reply_size > 20) {
            size_t copy = reply_size - 20;
            if (copy >= sizeof(context->local_name))
                copy = sizeof(context->local_name) - 1;
            memcpy(context->local_name, reply + 20, copy);
            context->local_name[copy] = '\0';
        }
    }
    context->last_info = time(NULL);
    context->enabled = (context->current_settings & MGMT_SETTING_POWERED) != 0;
    return 0;
}

static int wait_for_controller_info(struct bt_context *context)
{
    int attempt;

    for (attempt = 0; attempt < STARTUP_INFO_RETRY_COUNT; ++attempt) {
        if (refresh_info(context) == 0)
            return 0;
        if (attempt + 1 < STARTUP_INFO_RETRY_COUNT) {
            struct timespec delay = {
                .tv_sec = STARTUP_INFO_RETRY_DELAY_MS / 1000,
                .tv_nsec = (STARTUP_INFO_RETRY_DELAY_MS % 1000) * 1000000L,
            };
            le_log_warn("btd: controller-info retry %d/%d",
                        attempt + 1, STARTUP_INFO_RETRY_COUNT - 1);
            (void)nanosleep(&delay, NULL);
        }
    }
    le_log_error("btd: controller-info unavailable after retries");
    return -1;
}

static int controller_command(struct bt_context *context, uint16_t opcode,
                              const void *payload, size_t size)
{
    int result = mgmt_command(context, opcode, payload, size, NULL, NULL);
    if (!result)
        context->last_error[0] = '\0';
    return result;
}

static int set_powered(struct bt_context *context, int enabled)
{
    uint8_t value = enabled ? 1 : 0;
    int result = controller_command(context, MGMT_OP_SET_POWERED, &value, 1);

    if (!result)
        context->enabled = enabled;
    return result;
}

static int enable_link_security(struct bt_context *context)
{
    uint8_t enabled = 1;

    if (!(context->supported_settings & MGMT_SETTING_LINK_SECURITY)) {
        snprintf(context->last_error, sizeof(context->last_error),
                 "hci0 does not support BR/EDR link security");
        return -1;
    }
    if (context->current_settings & MGMT_SETTING_LINK_SECURITY)
        return 0;
    if (controller_command(context, MGMT_OP_SET_LINK_SECURITY, &enabled,
                           sizeof(enabled)) != 0) {
        snprintf(context->last_error, sizeof(context->last_error),
                 "hci0 BR/EDR link security could not be enabled");
        return -1;
    }
    context->current_settings |= MGMT_SETTING_LINK_SECURITY;
    return 0;
}

static int enable_secure_simple_pairing(struct bt_context *context)
{
    uint8_t enabled = 1;

    if (!(context->supported_settings & MGMT_SETTING_SSP)) {
        snprintf(context->last_error, sizeof(context->last_error),
                 "hci0 does not support Secure Simple Pairing");
        return -1;
    }
    if (context->current_settings & MGMT_SETTING_SSP)
        return 0;
    if (controller_command(context, MGMT_OP_SET_SSP, &enabled,
                           sizeof(enabled)) != 0) {
        snprintf(context->last_error, sizeof(context->last_error),
                 "hci0 Secure Simple Pairing could not be enabled");
        return -1;
    }
    context->current_settings |= MGMT_SETTING_SSP;
    return 0;
}

static int status_json(struct bt_context *context, char *data, size_t size)
{
    size_t used = 0;
    size_t i;
    size_t bonded_used = 0;
    size_t list_size;
    int available;
    char escaped[BT_NAME_MAX * 2];
    char bonded[LE_ADAPTER_MSG_MAX];
    char local_name[BT_NAME_MAX * 2];
    const char *profile_state;
    const char *profile_error;
    int sdp = 0, a2dp = 0, avrcp = 0;
    bonded[0] = '\0';

    if (hci_present() && (time(NULL) - context->last_info > 2))
        (void)refresh_info(context);
    refresh_connected_rssi(context);
    json_escape(context->local_name, local_name, sizeof(local_name));
    available = wmt_present() && access(BT_DEVICE, F_OK) == 0 &&
                hci_present() && context->mgmt_fd >= 0;
    if (context->profiles_opened) {
        sdp = le_profile_registered_sdp(&context->profiles);
        a2dp = le_profile_registered_a2dp_sink(&context->profiles);
        avrcp = le_profile_registered_avrcp(&context->profiles);
    }
    if (sdp && a2dp && avrcp) {
        profile_state = "ready";
        profile_error = "SDP, A2DP sink, and AVRCP profile services registered";
    } else if (sdp || a2dp || avrcp) {
        profile_state = "partial";
        profile_error = "Some Bluetooth profile services could not register";
    } else {
        profile_state = "pairing-only";
        profile_error = "No userspace Bluetooth profile service is registered";
    }
    if (append_text(data, size, &used,
                    "{\"state\":\"%s\",\"available\":%s,"
                    "\"enabled\":%s,\"activation_attempted\":%s,"
                    "\"transport\":\"mt8163-btif-hci\",\"hci\":\"%s\","
                    "\"last_error\":\"%s\","
                    "\"last_disconnect_reason\":\"%s\","
                    "\"last_connect_failed_status\":\"%s\","
                    "\"scanning\":%s,\"pairing\":%s,\"pairing_mode\":%s,\"local_name\":\"%s\","
                    "\"capabilities\":{\"classic\":%s,\"le\":%s,"
                    "\"ssp\":%s,\"secure_connection\":%s,"
                    "\"connectable\":%s,\"discoverable\":%s,"
                    "\"bondable\":%s,\"profile_state\":\"%s\",\"profile_error\":\"%s\","
                    "\"profile_services\":{\"sdp\":%s,\"a2dp_sink\":%s,\"avrcp\":%s,"
                    "\"rfcomm\":%s,\"bnep\":%s,\"hidp\":%s}},"
                    "\"pending_pairing\":",
                    context->enabled ? "up" :
                    context->activation_attempted ? "activation-attempted" :
                    available ? "ready" : "unavailable",
                    available ? "true" : "false", context->enabled ? "true" : "false",
                    context->activation_attempted ? "true" : "false",
                    context->enabled ? "hci0" : "none", context->last_error,
                    context->last_disconnect_reason,
                    context->last_connect_failed_status,
                    context->scanning ? "true" : "false",
                    context->pairing.active ? "true" : "false",
                    context->pairing_mode ? "true" : "false",
                    local_name,
                    context->supported_settings & MGMT_SETTING_BREDR ? "true" : "false",
                    context->supported_settings & MGMT_SETTING_LE ? "true" : "false",
                    context->supported_settings & MGMT_SETTING_SSP ? "true" : "false",
                    context->supported_settings & MGMT_SETTING_SECURE_CONN ? "true" : "false",
                    context->current_settings & MGMT_SETTING_CONNECTABLE ? "true" : "false",
                    context->current_settings & MGMT_SETTING_DISCOVERABLE ? "true" : "false",
                    context->current_settings & MGMT_SETTING_BONDABLE ? "true" : "false",
                    profile_state, profile_error,
                    sdp ? "true" : "false", a2dp ? "true" : "false",
                    avrcp ? "true" : "false",
                    "false", "false", "false") != 0)
        return -1;
    if (!context->pairing.active) {
        if (append_text(data, size, &used, "null,\"known_devices\":[") != 0)
            return -1;
    } else {
        char address[18];
        address_text(context->pairing.address, address, sizeof(address));
        if (append_text(data, size, &used,
                        "{\"address\":\"%s\",\"type\":%u,\"method\":\"%s\","
                        "\"value\":%u},\"known_devices\":[",
                        address, context->pairing.type, context->pairing.method,
                        context->pairing.value) != 0)
            return -1;
    }
    /*
     * Bonds are written before discovery results.  The bonded set is bounded by
     * what the user paired and is the list they act on, so the elastic
     * discovery list is the one that gives way when the message runs out.
     */
    {
        int first = 1;
        for (i = 0; i < context->device_count; ++i) {
            char address[18];
            char bond_name[BT_STATUS_BOND_NAME_MAX * 2 + 4];
            if (!context->devices[i].paired)
                continue;
            address_text(context->devices[i].address, address, sizeof(address));
            bond_name_json(context->devices[i].name, bond_name,
                           sizeof(bond_name));
            if (append_text(bonded, sizeof(bonded), &bonded_used,
                            "%s{\"address\":\"%s\",\"name\":\"%s\","
                            "\"type\":%u,\"rssi\":%d,\"rssi_valid\":%s,\"connected\":%s}",
                            first ? "" : ",", address, bond_name,
                            context->devices[i].type, context->devices[i].rssi,
                            context->devices[i].rssi_valid ? "true" : "false",
                            context->devices[i].connected ? "true" : "false") != 0) {
                le_log_error("btd: bonded-device status exceeds bounded message");
                return -1;
            }
            first = 0;
        }
    }
    if (append_text(data, size, &used, "%s],\"discovered\":[", bonded) != 0)
        return -1;
    list_size = size > STATUS_ENVELOPE_RESERVE ?
                size - STATUS_ENVELOPE_RESERVE : used;
    for (i = 0; i < context->device_count; ++i) {
        char address[18], name[BT_NAME_MAX * 2];
        if (!context->devices[i].discovered)
            continue;
        address_text(context->devices[i].address, address, sizeof(address));
        json_escape(context->devices[i].name, escaped, sizeof(escaped));
        strncpy(name, escaped, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        if (append_text(data, list_size, &used,
                        "%s{\"address\":\"%s\",\"name\":\"%s\","
                        "\"type\":%u,\"rssi\":%d,\"rssi_valid\":%s,\"paired\":%s,"
                        "\"connected\":%s}",
                        used && data[used - 1] != '[' ? "," : "", address, name,
                        context->devices[i].type, context->devices[i].rssi,
                        context->devices[i].rssi_valid ? "true" : "false",
                        context->devices[i].paired ? "true" : "false",
                        context->devices[i].connected ? "true" : "false") != 0) {
            le_log_warn("btd: discovery list clipped to the bounded status message");
            break;
        }
    }
    return append_text(data, size, &used, "]}");
}

static int remove_keys(struct bt_context *context, const uint8_t *address,
                       uint8_t type)
{
    size_t i, out = 0;
    for (i = 0; i < context->link_key_count; ++i)
        if (!same_address(context->link_keys[i].key.address.address, address) ||
            context->link_keys[i].key.address.type != type)
            context->link_keys[out++] = context->link_keys[i];
    context->link_key_count = out;
    out = 0;
    for (i = 0; i < context->ltk_count; ++i)
        if (!same_address(context->ltks[i].key.address.address, address) ||
            context->ltks[i].key.address.type != type)
            context->ltks[out++] = context->ltks[i];
    context->ltk_count = out;
    return save_keys(context);
}

static int set_discovery(struct bt_context *context, int start)
{
    uint8_t type = MGMT_DISCOVERY_INTERLEAVED;
    size_t i;
    int result;

    if (!context->enabled)
        return -1;
    if (start) {
        size_t kept = 0;
        /*
         * Discovery is a live view, not a history.  LE peers advertise with
         * rotating resolvable private addresses, so every scan mints new table
         * entries; keeping them fills the bounded table, after which
         * get_device() returns NULL and no further device can be reported for
         * the rest of the boot.  Drop the previous scan's unbonded,
         * disconnected entries so each scan starts from the bonded set.
         */
        for (i = 0; i < context->device_count; ++i) {
            if (!context->devices[i].paired && !context->devices[i].connected)
                continue;
            context->devices[i].discovered = 0;
            context->devices[kept++] = context->devices[i];
        }
        context->device_count = kept;
        result = controller_command(context, MGMT_OP_START_DISCOVERY, &type, 1);
        /*
         * This 3.18 port can report LE capability while the MT8163 HCI
         * initialization leaves LE disabled.  Interleaved discovery is
         * rejected in that state even though classic inquiry is usable.
         * Fall back only for capability/parameter rejection; preserve busy
         * and controller failures for the caller.
         */
        if (result == -(int)MGMT_STATUS_REJECTED ||
            result == -(int)MGMT_STATUS_NOT_SUPPORTED ||
            result == -(int)MGMT_STATUS_INVALID_PARAMS) {
            type = MGMT_DISCOVERY_BREDR;
            le_log_warn("btd: interleaved discovery unavailable; trying classic inquiry");
            result = controller_command(context, MGMT_OP_START_DISCOVERY,
                                        &type, 1);
        }
        if (!result) {
            context->discovery_type = type;
            context->scanning = 1;
        }
    } else {
        if (context->discovery_type)
            type = context->discovery_type;
        result = controller_command(context, MGMT_OP_STOP_DISCOVERY, &type, 1);
        if (!result) {
            context->scanning = 0;
            context->discovery_type = 0;
        }
    }
    return result;
}

static int set_controller_setting(struct bt_context *context, uint16_t opcode,
                                  int enabled)
{
    uint8_t payload[3];
    int result;

    memset(payload, 0, sizeof(payload));
    payload[0] = enabled ? 1 : 0;
    result = controller_command(context, opcode, payload,
                                 opcode == MGMT_OP_SET_DISCOVERABLE ?
                                 sizeof(payload) : 1);
    if (!result) {
        if (enabled)
            context->current_settings |= opcode == MGMT_OP_SET_CONNECTABLE ?
                MGMT_SETTING_CONNECTABLE : opcode == MGMT_OP_SET_DISCOVERABLE ?
                MGMT_SETTING_DISCOVERABLE : MGMT_SETTING_BONDABLE;
        else
            context->current_settings &= opcode == MGMT_OP_SET_CONNECTABLE ?
                ~MGMT_SETTING_CONNECTABLE : opcode == MGMT_OP_SET_DISCOVERABLE ?
                ~MGMT_SETTING_DISCOVERABLE : ~MGMT_SETTING_BONDABLE;
    }
    return result;
}

static int set_pairing_identity(struct bt_context *context)
{
    uint8_t name[MGMT_SET_LOCAL_NAME_SIZE];
    uint8_t device_class[2] = {
        LIBREECHO_BT_MAJOR_CLASS,
        LIBREECHO_BT_MINOR_CLASS,
    };
    uint8_t uuid_payload[17];

    memset(name, 0, sizeof(name));
    memcpy(name, LIBREECHO_BT_NAME, sizeof(LIBREECHO_BT_NAME) - 1);

    if (controller_command(context, MGMT_OP_SET_LOCAL_NAME, name,
                            sizeof(name)) != 0) {
        le_log_warn("btd: could not publish pairing name");
        return -1;
    }
    if (!context->audio_sink_uuid_published) {
        memcpy(uuid_payload, libreecho_audio_sink_uuid,
               sizeof(libreecho_audio_sink_uuid));
        uuid_payload[16] = 0x24;
        if (controller_command(context, MGMT_OP_ADD_UUID, uuid_payload,
                                sizeof(uuid_payload)) != 0) {
            le_log_warn("btd: could not publish Audio Sink service UUID");
            return -1;
        }
        context->audio_sink_uuid_published = 1;
    }
    if (controller_command(context, MGMT_OP_SET_DEV_CLASS, device_class,
                            sizeof(device_class)) != 0) {
        le_log_warn("btd: could not publish pairing device class");
        return -1;
    }
    strncpy(context->local_name, LIBREECHO_BT_NAME,
            sizeof(context->local_name) - 1);
    context->local_name[sizeof(context->local_name) - 1] = '\0';
    le_log_info("btd: pairing identity published (name=%s, class=audio-rendering)",
                LIBREECHO_BT_NAME);
    return 0;
}

static int set_pairing_mode(struct bt_context *context, int enabled)
{
    int result;

    if (!context->enabled)
        return -1;
    if (enabled == context->pairing_mode)
        return 0;
    if (!context->capability_ready) {
        uint8_t io_capability = MGMT_IO_CAP_DISPLAY_YES_NO;

        if (enable_secure_simple_pairing(context) != 0) {
            (void)set_powered(context, 0);
            context->enabled = 0;
            context->capability_ready = 0;
            return -1;
        }
        if (controller_command(context, MGMT_OP_SET_IO_CAPABILITY,
                               &io_capability, sizeof(io_capability)) != 0) {
            (void)set_powered(context, 0);
            context->enabled = 0;
            context->capability_ready = 0;
            snprintf(context->last_error, sizeof(context->last_error),
                     "hci0 IO capability could not be configured");
            return -1;
        }
        context->capability_ready = 1;
        set_controller_ready(1);
    }
    if (enabled) {
        context->pairing_saved_connectable =
            (context->current_settings & MGMT_SETTING_CONNECTABLE) != 0;
        context->pairing_saved_discoverable =
            (context->current_settings & MGMT_SETTING_DISCOVERABLE) != 0;
        context->pairing_saved_bondable =
            (context->current_settings & MGMT_SETTING_BONDABLE) != 0;
        if (context->scanning)
            (void)set_discovery(context, 0);
        if (set_pairing_identity(context) != 0) {
            snprintf(context->last_error, sizeof(context->last_error),
                     "Bluetooth pairing identity could not be configured");
            return -1;
        }
        result = set_controller_setting(context, MGMT_OP_SET_BONDABLE, 1);
        if (!result)
            result = set_controller_setting(context, MGMT_OP_SET_CONNECTABLE, 1);
        if (!result)
            result = set_controller_setting(context, MGMT_OP_SET_DISCOVERABLE, 1);
        if (result) {
            (void)set_controller_setting(context, MGMT_OP_SET_BONDABLE,
                                          context->pairing_saved_bondable);
            (void)set_controller_setting(context, MGMT_OP_SET_CONNECTABLE,
                                          context->pairing_saved_connectable);
            (void)set_controller_setting(context, MGMT_OP_SET_DISCOVERABLE,
                                          context->pairing_saved_discoverable);
            return result;
        }
        context->pairing_mode = 1;
        led_pattern("pulse", 255, 180, 0, 70, 0);
        le_log_info("btd: pairing mode enabled (connectable, discoverable, bondable)");
        return 0;
    }
    result = set_controller_setting(context, MGMT_OP_SET_DISCOVERABLE,
                                    context->pairing_saved_discoverable);
    if (!result)
        result = set_controller_setting(context, MGMT_OP_SET_CONNECTABLE,
                                        context->pairing_saved_connectable);
    if (!result)
        result = set_controller_setting(context, MGMT_OP_SET_BONDABLE,
                                        context->pairing_saved_bondable);
    if (!result) {
        context->pairing_mode = 0;
        led_pattern("stop", 0, 0, 0, 0, 0);
        le_log_info("btd: pairing mode disabled");
    }
    return result;
}

static int handle_request(struct bt_context *context, char *message,
                          char *response, size_t response_size)
{
    char command[64], value[128], data[MGMT_PACKET_MAX];
    char *args;
    unsigned long id;
    int type = 0, io_cap = MGMT_IO_CAP_NO_INPUT_NO_OUTPUT;

    if (le_adapter_parse_request(message, command, sizeof(command), &args,
                                 &id) != 0)
        return le_adapter_respond_err(response, response_size, 0,
                                       "malformed request");
    le_log_debug("btd: cmd=\"%s\" id=%lu", command, id);
    if (!strcmp(command, "status")) {
        if (status_json(context, data, sizeof(data)) != 0)
            return le_adapter_respond_err(response, response_size, id,
                                           "Bluetooth status is too large");
        return le_adapter_respond_ok(response, response_size, id, data);
    }
    if (!strcmp(command, "set_enabled")) {
        static const char *const configure[] = {
            CONFIGURE_HELPER, "--device", WMT_DEVICE, "--firmware-dir",
            FIRMWARE_DIR, NULL
        };
        static const char *const bt_on[] = {
            BT_ON_HELPER, "--device", WMT_DEVICE,
            "--execute-bt-only-once", NULL
        };
        int enabled;
        if (json_integer(args, "enabled", &enabled) != 0) {
            const char *p = json_value(args, "enabled");
            if (!p || (strncmp(p, "true", 4) && strncmp(p, "false", 5)))
                return le_adapter_respond_err(response, response_size, id,
                                               "enabled must be boolean");
            enabled = !strncmp(p, "true", 4);
        }
        if (!enabled) {
            if (context->pairing_mode)
                (void)set_pairing_mode(context, 0);
            if (context->enabled && set_powered(context, 0) != 0)
                return le_adapter_respond_err(response, response_size, id,
                                               "Bluetooth power-off failed");
            context->enabled = 0;
            context->capability_ready = 0;
            return le_adapter_respond_ok(response, response_size, id,
                                         "{\"enabled\":false}");
        }
        if (!wmt_present())
            return le_adapter_respond_err(response, response_size, id,
                                           "MT8163 WMT device is unavailable");
        if (context->enabled) {
            uint8_t io_capability = MGMT_IO_CAP_DISPLAY_YES_NO;

            if (context->capability_ready)
                return le_adapter_respond_ok(response, response_size, id,
                                             "{\"enabled\":true}");
            if (enable_link_security(context) != 0) {
                (void)set_powered(context, 0);
                context->enabled = 0;
                context->capability_ready = 0;
                return le_adapter_respond_err(response, response_size, id,
                                               context->last_error);
            }
            if (enable_secure_simple_pairing(context) != 0) {
                (void)set_powered(context, 0);
                context->enabled = 0;
                context->capability_ready = 0;
                return le_adapter_respond_err(response, response_size, id,
                                               context->last_error);
            }
            if (controller_command(context, MGMT_OP_SET_IO_CAPABILITY,
                                   &io_capability, sizeof(io_capability)) != 0) {
                (void)set_powered(context, 0);
                context->enabled = 0;
                context->capability_ready = 0;
                snprintf(context->last_error, sizeof(context->last_error),
                         "hci0 IO capability could not be configured");
                return le_adapter_respond_err(response, response_size, id,
                                               context->last_error);
            }
            context->capability_ready = 1;
            set_controller_ready(1);
            return le_adapter_respond_ok(response, response_size, id,
                                         "{\"enabled\":true}");
        }
        if (context->activation_attempted)
            return le_adapter_respond_err(response, response_size, id,
                                           "activation already attempted; reboot before retry");
        context->activation_attempted = 1;
        le_log_info("btd: explicit Bluetooth activation requested");
        if (run_helper(CONFIGURE_HELPER, "wmt_configure", configure) != 0) {
            snprintf(context->last_error, sizeof(context->last_error),
                     "wmt_configure failed");
            return le_adapter_respond_err(response, response_size, id,
                                           context->last_error);
        }
        if (run_helper(BT_ON_HELPER, "wmt_bt_on", bt_on) != 0) {
            snprintf(context->last_error, sizeof(context->last_error),
                     "wmt_bt_on failed");
            return le_adapter_respond_err(response, response_size, id,
                                           context->last_error);
        }
        /* WMT has prepared BTIF; the supported Linux 6.1 management
         * SET_POWERED command performs the HCI open transition.  Do not use
         * the obsolete HCIDEVUP ioctl here: this target's HCI raw socket does
         * not implement that legacy command and returns ENOTTY, leaving the
         * virtual controller down before the first MGMT write. */
        if (set_powered(context, 1) != 0 ||
            wait_for_controller_info(context) != 0) {
            snprintf(context->last_error, sizeof(context->last_error),
                     "hci0 power-on failed");
            return le_adapter_respond_err(response, response_size, id,
                                           context->last_error);
        }
        {
            uint8_t io_capability = MGMT_IO_CAP_DISPLAY_YES_NO;
            if (enable_link_security(context) != 0) {
                (void)set_powered(context, 0);
                context->enabled = 0;
                context->capability_ready = 0;
                return le_adapter_respond_err(response, response_size, id,
                                               context->last_error);
            }
            if (enable_secure_simple_pairing(context) != 0) {
                (void)set_powered(context, 0);
                context->enabled = 0;
                context->capability_ready = 0;
                return le_adapter_respond_err(response, response_size, id,
                                               context->last_error);
            }
            if (controller_command(context, MGMT_OP_SET_IO_CAPABILITY,
                                   &io_capability, sizeof(io_capability)) != 0) {
                (void)set_powered(context, 0);
                context->enabled = 0;
                context->capability_ready = 0;
                snprintf(context->last_error, sizeof(context->last_error),
                         "hci0 IO capability could not be configured");
                return le_adapter_respond_err(response, response_size, id,
                                               context->last_error);
            }
        }
        context->capability_ready = 1;
        set_controller_ready(1);
        /* Opening the management channel clears HCI_BONDABLE.  The legacy
         * HCIDEVUP path deliberately does not restore it once management is
         * active, so an incoming peer would otherwise receive an immediate
         * PIN_CODE_NEG_REPLY and no pairing request would reach the UI. */
        if (set_controller_setting(context, MGMT_OP_SET_BONDABLE, 1) != 0) {
            snprintf(context->last_error, sizeof(context->last_error),
                     "hci0 bondable mode could not be enabled");
            return le_adapter_respond_err(response, response_size, id,
                                           context->last_error);
        }
        if (set_controller_setting(context, MGMT_OP_SET_CONNECTABLE, 1) != 0) {
            snprintf(context->last_error, sizeof(context->last_error),
                     "hci0 connectable mode could not be enabled");
            return le_adapter_respond_err(response, response_size, id,
                                           context->last_error);
        }
        (void)load_keys_into_controller(context);
        context->last_error[0] = '\0';
        if (status_json(context, data, sizeof(data)) != 0)
            return -1;
        return le_adapter_respond_ok(response, response_size, id, data);
    }
    if (!strcmp(command, "pairing_mode")) {
        int enabled;
        if (json_integer(args, "enabled", &enabled) != 0) {
            const char *p = json_value(args, "enabled");
            if (!p || (strncmp(p, "true", 4) && strncmp(p, "false", 5)))
                return le_adapter_respond_err(response, response_size, id,
                                               "enabled must be boolean");
            enabled = !strncmp(p, "true", 4);
        }
        if (enabled != 0 && enabled != 1)
            return le_adapter_respond_err(response, response_size, id,
                                           "enabled must be 0 or 1");
        if (set_pairing_mode(context, enabled) != 0)
            return le_adapter_respond_err(response, response_size, id,
                                           "Bluetooth pairing mode could not be changed");
        return le_adapter_respond_ok(response, response_size, id,
                                     enabled ? "{\"pairing_mode\":true}" :
                                     "{\"pairing_mode\":false}");
    }
    if (!strcmp(command, "scan") || !strcmp(command, "scan_stop")) {
        if (set_discovery(context, !strcmp(command, "scan")) != 0)
            return le_adapter_respond_err(response, response_size, id,
                                           "Bluetooth discovery command failed");
        return le_adapter_respond_ok(response, response_size, id,
                                     !strcmp(command, "scan") ?
                                     "{\"scanning\":true}" :
                                     "{\"scanning\":false}");
    }
    if (!strcmp(command, "pair") || !strcmp(command, "unpair") ||
        !strcmp(command, "disconnect") || !strcmp(command, "pair_confirm") ||
        !strcmp(command, "pair_reject") || !strcmp(command, "pair_passkey") ||
        !strcmp(command, "pair_pin")) {
        struct mgmt_addr_wire address_info;
        memset(&address_info, 0, sizeof(address_info));
        if (json_string(args, "address", value, sizeof(value)) != 0 ||
            parse_address(value, address_info.address) != 0)
            return le_adapter_respond_err(response, response_size, id,
                                           "A valid Bluetooth address is required");
        if (json_integer(args, "type", &type) == 0 && (type < 0 || type > 255))
            return le_adapter_respond_err(response, response_size, id,
                                           "Bluetooth address type is invalid");
        address_info.type = (uint8_t)type;
        if (!strcmp(command, "pair")) {
            struct { struct mgmt_addr_wire address; uint8_t io_cap; }
                __attribute__((packed)) pair = { address_info, (uint8_t)io_cap };
            (void)json_integer(args, "io_capability", &io_cap);
            pair.io_cap = (uint8_t)io_cap;
            if (controller_command(context, MGMT_OP_PAIR_DEVICE, &pair,
                                    sizeof(pair)) != 0)
                return le_adapter_respond_err(response, response_size, id,
                                               "Bluetooth pairing could not start");
            return le_adapter_respond_ok(response, response_size, id,
                                         "{\"pairing\":true}");
        }
        if (!strcmp(command, "unpair")) {
            struct { struct mgmt_addr_wire address; uint8_t disconnect; }
                __attribute__((packed)) unpair = { address_info, 1 };
            if (controller_command(context, MGMT_OP_UNPAIR_DEVICE, &unpair,
                                    sizeof(unpair)) != 0)
                return le_adapter_respond_err(response, response_size, id,
                                               "Bluetooth unpair failed");
            {
                struct bt_device *device = find_device(context, address_info.address,
                                                       address_info.type);
                if (device)
                    device->paired = device->connected = 0;
            }
            (void)remove_keys(context, address_info.address, address_info.type);
            (void)save_devices(context);
            return le_adapter_respond_ok(response, response_size, id,
                                         "{\"paired\":false}");
        }
        if (!strcmp(command, "disconnect")) {
            if (controller_command(context, MGMT_OP_DISCONNECT, &address_info,
                                   sizeof(address_info)) != 0)
                return le_adapter_respond_err(response, response_size, id,
                                               "Bluetooth disconnect failed");
            return le_adapter_respond_ok(response, response_size, id,
                                         "{\"connected\":false}");
        }
        if (!strcmp(command, "pair_confirm") || !strcmp(command, "pair_reject")) {
            uint16_t opcode = !strcmp(command, "pair_confirm") ?
                              MGMT_OP_USER_CONFIRM_REPLY :
                              MGMT_OP_USER_CONFIRM_NEG_REPLY;
            if (controller_command(context, opcode, &address_info,
                                   sizeof(address_info)) != 0)
                return le_adapter_respond_err(response, response_size, id,
                                               "Bluetooth confirmation failed");
            pairing_clear(context);
            return le_adapter_respond_ok(response, response_size, id,
                                         "{\"pairing_response\":true}");
        }
        if (!strcmp(command, "pair_passkey")) {
            uint8_t passkey[sizeof(address_info) + 4];
            int passkey_value;
            if (json_integer(args, "passkey", &passkey_value) != 0 ||
                passkey_value < 0 || passkey_value > 999999)
                return le_adapter_respond_err(response, response_size, id,
                                               "A six-digit passkey is required");
            memcpy(passkey, &address_info, sizeof(address_info));
            write_le32(passkey + sizeof(address_info), (uint32_t)passkey_value);
            if (controller_command(context, MGMT_OP_USER_PASSKEY_REPLY, passkey,
                                   sizeof(passkey)) != 0)
                return le_adapter_respond_err(response, response_size, id,
                                               "Bluetooth passkey response failed");
            pairing_clear(context);
            return le_adapter_respond_ok(response, response_size, id,
                                         "{\"pairing_response\":true}");
        }
        if (json_string(args, "pin", value, sizeof(value)) != 0 ||
            strlen(value) > 16)
            return le_adapter_respond_err(response, response_size, id,
                                           "A PIN of up to 16 characters is required");
        {
            uint8_t pin[sizeof(address_info) + 17];
            size_t pin_length = strlen(value);
            memcpy(pin, &address_info, sizeof(address_info));
            pin[sizeof(address_info)] = (uint8_t)pin_length;
            memset(pin + sizeof(address_info) + 1, 0, 16);
            memcpy(pin + sizeof(address_info) + 1, value, pin_length);
            if (controller_command(context, MGMT_OP_PIN_CODE_REPLY, pin,
                                   sizeof(pin)) != 0)
                return le_adapter_respond_err(response, response_size, id,
                                               "Bluetooth PIN response failed");
        }
        pairing_clear(context);
        return le_adapter_respond_ok(response, response_size, id,
                                     "{\"pairing_response\":true}");
    }
    if (!strcmp(command, "set_connectable") || !strcmp(command, "set_discoverable")) {
        int enabled;
        uint8_t payload[3];
        uint16_t opcode = !strcmp(command, "set_connectable") ?
                          MGMT_OP_SET_CONNECTABLE : MGMT_OP_SET_DISCOVERABLE;
        if (json_integer(args, "enabled", &enabled) != 0) {
            const char *p = json_value(args, "enabled");
            if (!p || (strncmp(p, "true", 4) && strncmp(p, "false", 5)))
                return le_adapter_respond_err(response, response_size, id,
                                               "enabled must be boolean");
            enabled = !strncmp(p, "true", 4);
        }
        if (enabled != 0 && enabled != 1)
            return le_adapter_respond_err(response, response_size, id,
                                           "enabled must be 0 or 1");
        memset(payload, 0, sizeof(payload));
        payload[0] = enabled ? 1 : 0;
        if (controller_command(context, opcode, payload,
                               opcode == MGMT_OP_SET_DISCOVERABLE ?
                               sizeof(payload) : 1) != 0)
            return le_adapter_respond_err(response, response_size, id,
                                           "Bluetooth controller setting failed");
        return le_adapter_respond_ok(response, response_size, id,
                                     "{\"changed\":true}");
    }
    return le_adapter_respond_err(response, response_size, id, "unknown command");
}

static int serve_client(struct bt_context *context, int client)
{
    char message[LE_ADAPTER_MSG_MAX];
    char response[LE_ADAPTER_MSG_MAX];
    int length;

    if (read_line(client, message, sizeof(message)) != 0)
        return -1;
    length = handle_request(context, message, response, sizeof(response));
    if (length < 0)
        return -1;
    return write_all(client, response, (size_t)length);
}

static void drain_mgmt(struct bt_context *context)
{
    uint8_t packet[MGMT_PACKET_MAX];
    ssize_t count;

    if (context->mgmt_fd < 0)
        return;
    do {
        count = read(context->mgmt_fd, packet, sizeof(packet));
        if (count > 0)
            (void)process_packet(context, packet, (size_t)count, 0xffff, NULL,
                                 NULL);
    } while (count > 0);
}

int main(int argc, char **argv)
{
    const char *socket_path = DEFAULT_SOCKET;
    struct bt_context context;
    int listener;
    int i;

    le_log_init("btd", argc, argv);
    memset(&context, 0, sizeof(context));
    context.mgmt_fd = -1;
    load_devices(&context);
    load_keys(&context);
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--foreground"))
            continue;
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            socket_path = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--help")) {
            printf("Usage: %s [--foreground] [--socket PATH]\n", argv[0]);
            return 0;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return 2;
    }
    listener = le_adapter_listen(socket_path);
    if (listener < 0) {
        le_log_error("btd: cannot listen on %s: %s", socket_path, strerror(errno));
        return 1;
    }
    if (mgmt_open(&context) != 0) {
        le_log_error("btd: Bluetooth management channel unavailable");
        close(listener);
        return 1;
    }
    if (access(HCI_DEVICE, F_OK) == 0 && refresh_info(&context) != 0)
        le_log_warn("btd: controller not ready; activation remains available");
    if (context.enabled && !context.capability_ready) {
        uint8_t io_capability = MGMT_IO_CAP_DISPLAY_YES_NO;

        if (enable_link_security(&context) != 0) {
            (void)set_powered(&context, 0);
            context.enabled = 0;
            context.capability_ready = 0;
            close(listener);
            return 1;
        }
        if (enable_secure_simple_pairing(&context) != 0) {
            (void)set_powered(&context, 0);
            context.enabled = 0;
            context.capability_ready = 0;
            close(listener);
            return 1;
        }
        if (controller_command(&context, MGMT_OP_SET_IO_CAPABILITY,
                               &io_capability, sizeof(io_capability)) != 0) {
            (void)set_powered(&context, 0);
            context.enabled = 0;
            context.capability_ready = 0;
            snprintf(context.last_error, sizeof(context.last_error),
                     "hci0 IO capability could not be configured");
            close(listener);
            return 1;
        }
        context.capability_ready = 1;
        set_controller_ready(1);
    }
    (void)load_keys_into_controller(&context);
    if (le_profile_open(&context.profiles, LIBREECHO_BT_NAME) == 0) {
        context.profiles_opened = 1;
        le_log_info("btd: Bluetooth profile services registered");
    } else {
        le_log_warn("btd: Bluetooth profile services unavailable: %s",
                    strerror(errno));
    }
    signal(SIGINT, stop_daemon);
    signal(SIGTERM, stop_daemon);
    signal(SIGPIPE, SIG_IGN);
    le_log_info("btd: listening on %s (manual activation, HCI management enabled)",
                socket_path);
    while (!stopping) {
        struct pollfd pollfds[2 + LE_PROFILE_MAX_FDS];
        int fd_map[LE_PROFILE_MAX_FDS];
        int profile_count = 0;
        int profile_base = 2;
        int total;
        int i;

        pollfds[0].fd = listener;
        pollfds[0].events = POLLIN;
        total = 1;
        if (context.mgmt_fd >= 0) {
            pollfds[1].fd = context.mgmt_fd;
            pollfds[1].events = POLLIN;
            total = 2;
        } else {
            profile_base = 1;
        }
        if (context.profiles_opened) {
            profile_count = le_profile_poll_setup(&context.profiles,
                                                  pollfds + profile_base,
                                                  LE_PROFILE_MAX_FDS, fd_map);
        }
        if (poll(pollfds, (nfds_t)(total + profile_count), 500) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (total == 2 && (pollfds[1].revents & POLLIN))
            drain_mgmt(&context);
        if (profile_count)
            le_profile_poll_events(&context.profiles, pollfds + profile_base,
                                   fd_map, profile_count);
        if (pollfds[0].revents & POLLIN) {
            int client = le_adapter_accept(listener);
            if (client < 0) {
                if (errno != EINTR)
                    le_log_warn("btd: accept failed: %s", strerror(errno));
            } else {
                (void)serve_client(&context, client);
                close(client);
            }
        }
    }
    close(listener);
    if (context.mgmt_fd >= 0)
        close(context.mgmt_fd);
    if (context.profiles_opened)
        le_profile_close(&context.profiles);
    unlink(socket_path);
    le_log_info("btd: stopped");
    return 0;
}
