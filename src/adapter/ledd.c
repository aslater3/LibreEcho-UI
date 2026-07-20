/*
 * LibreEcho LED companion daemon.
 *
 * The protocol is deliberately implemented here rather than depending on a
 * JSON library: this daemon is also intended to run on the small musl-based
 * userspace shipped by the Echo Linux port.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "adapter.h"
#include "log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef I2C_SLAVE
/* Linux i2c-dev UAPI value; avoid requiring linux/i2c-dev.h on musl SDKs. */
#define I2C_SLAVE 0x0703
#endif

#define MAX_CLIENTS 4
#define STATE_PATH "/etc/libreecho/led-state.json"
#define SYSFS_LED_DIR "/sys/class/leds"
#define MAX_PATH 512
#define FRAME_MS 33

struct colour {
    unsigned int r;
    unsigned int g;
    unsigned int b;
    unsigned int brightness;
};

struct led_state {
    struct colour current;
    struct colour boot;
    struct colour profiles[4];
};

enum profile_id {
    PROFILE_LISTENING = 0,
    PROFILE_THINKING,
    PROFILE_ERROR,
    PROFILE_DND,
    PROFILE_COUNT
};

static const char *const profile_names[PROFILE_COUNT] = {
    "listening", "thinking", "error", "dnd"
};

enum hardware_kind {
    HW_STUB = 0,
    HW_SYSFS,
    HW_I2C_STUB
};

struct hardware {
    enum hardware_kind kind;
    int has_multi;
    char multi_path[MAX_PATH];
    char red_path[MAX_PATH];
    char green_path[MAX_PATH];
    char blue_path[MAX_PATH];
    char brightness_path[MAX_PATH];
};

struct client {
    int fd;
    size_t length;
    char input[LE_ADAPTER_MSG_MAX + 1];
};

struct json_span {
    const char *start;
    const char *end;
};

struct request {
    unsigned long id;
    int version;
    char command[32];
    struct json_span args;
    int have_args;
};

struct daemon_context {
    int listen_fd;
    char socket_path[LE_ADAPTER_PATH_MAX];
    struct hardware hw;
    struct led_state state;
    struct client clients[MAX_CLIENTS];
    int foreground;
    int test_active;
    double test_started;
    struct colour test_saved;
    int test_saved_animation;
    int animation_active;
    int animation_profile;
    double animation_started;
};

static volatile sig_atomic_t stop_requested;

/* logging provided by le_log (src/log.h) */

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static double monotonic_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

/* A small sine approximation avoids pulling libm into the ARM32 binary. */
static double sine_approx(double x)
{
    const double pi = 3.14159265358979323846;
    const double two_pi = 6.28318530717958647692;
    double y;

    while (x > pi)
        x -= two_pi;
    while (x < -pi)
        x += two_pi;
    y = (4.0 / pi) * x - (4.0 / (pi * pi)) * x * (x < 0.0 ? -x : x);
    /* A correction term improves the shape around the zero crossings. */
    return 0.225 * (y * (y < 0.0 ? -y : y) - y) + y;
}

static int path_is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int append_path(char *out, size_t out_size, const char *dir,
                       const char *name)
{
    int n = snprintf(out, out_size, "%s/%s", dir, name);
    return n >= 0 && (size_t)n < out_size;
}

static int write_all_fd(int fd, const char *data, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, data + done, length - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int write_text_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    int rc;
    if (fd < 0)
        return -1;
    rc = write_all_fd(fd, text, strlen(text));
    if (close(fd) != 0)
        rc = -1;
    return rc;
}

static int write_number_file(const char *path, unsigned int value)
{
    char text[32];
    int n = snprintf(text, sizeof(text), "%u\n", value);
    if (n < 0 || (size_t)n >= sizeof(text))
        return -1;
    return write_text_file(path, text);
}

/* ---------------------------- Hardware layer --------------------------- */

static unsigned int scale_channel(unsigned int channel, unsigned int brightness)
{
    return (channel * brightness + 50U) / 100U;
}

static int hardware_write_rgb(const struct hardware *hw, unsigned int r,
                              unsigned int g, unsigned int b,
                              unsigned int brightness)
{
    char text[80];
    int errors = 0;

    if (hw->kind != HW_SYSFS)
        return 0;

    if (hw->has_multi) {
        int n = snprintf(text, sizeof(text), "%u %u %u\n", r, g, b);
        if (n < 0 || (size_t)n >= sizeof(text) ||
            write_text_file(hw->multi_path, text) != 0) {
            le_log_warn( "unable to write %s", hw->multi_path);
            errors++;
        }
        if (hw->brightness_path[0] != '\0' &&
            write_number_file(hw->brightness_path,
                              (brightness * 255U + 50U) / 100U) != 0) {
            le_log_warn( "unable to write %s", hw->brightness_path);
            errors++;
        }
    } else {
        if (hw->red_path[0] != '\0' &&
            write_number_file(hw->red_path, scale_channel(r, brightness)) != 0)
            errors++;
        if (hw->green_path[0] != '\0' &&
            write_number_file(hw->green_path, scale_channel(g, brightness)) != 0)
            errors++;
        if (hw->blue_path[0] != '\0' &&
            write_number_file(hw->blue_path, scale_channel(b, brightness)) != 0)
            errors++;
        /* Individual colour LEDs have already received the scaled value. */
        if (hw->brightness_path[0] != '\0' &&
            write_number_file(hw->brightness_path, 255U) != 0)
            errors++;
    }
    return errors ? -1 : 0;
}

static void hardware_apply(const struct hardware *hw, const struct colour *c)
{
    if (hardware_write_rgb(hw, c->r, c->g, c->b, c->brightness) != 0)
        le_log_warn( "LED hardware write failed; retaining state in memory");
}

static int contains_ci(const char *haystack, const char *needle)
{
    size_t i, j, n = strlen(needle);
    if (n == 0)
        return 1;
    for (i = 0; haystack[i] != '\0'; i++) {
        for (j = 0; j < n && haystack[i + j] != '\0' &&
                    tolower((unsigned char)haystack[i + j]) ==
                    tolower((unsigned char)needle[j]); j++) {
        }
        if (j == n)
            return 1;
    }
    return 0;
}

static void detect_sysfs(struct hardware *hw)
{
    DIR *dir;
    struct dirent *entry;
    char base[MAX_PATH];
    char path[MAX_PATH];
    char candidate[MAX_PATH];
    int have_red = 0, have_green = 0, have_blue = 0;

    dir = opendir(SYSFS_LED_DIR);
    if (dir == NULL)
        return;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        if (!append_path(base, sizeof(base), SYSFS_LED_DIR, entry->d_name))
            continue;

        if (!hw->has_multi &&
            append_path(path, sizeof(path), base, "multi_intensity") &&
            path_is_file(path)) {
            strncpy(hw->multi_path, path, sizeof(hw->multi_path) - 1);
            hw->multi_path[sizeof(hw->multi_path) - 1] = '\0';
            hw->has_multi = 1;
            if (append_path(path, sizeof(path), base, "brightness") &&
                path_is_file(path)) {
                strncpy(hw->brightness_path, path,
                        sizeof(hw->brightness_path) - 1);
                hw->brightness_path[sizeof(hw->brightness_path) - 1] = '\0';
            }
        }

        if (contains_ci(entry->d_name, "red") && !have_red &&
            append_path(candidate, sizeof(candidate), base, "brightness") &&
            path_is_file(candidate)) {
            strncpy(hw->red_path, candidate, sizeof(hw->red_path) - 1);
            hw->red_path[sizeof(hw->red_path) - 1] = '\0';
            have_red = 1;
        }
        if (contains_ci(entry->d_name, "green") && !have_green &&
            append_path(candidate, sizeof(candidate), base, "brightness") &&
            path_is_file(candidate)) {
            strncpy(hw->green_path, candidate, sizeof(hw->green_path) - 1);
            hw->green_path[sizeof(hw->green_path) - 1] = '\0';
            have_green = 1;
        }
        if (contains_ci(entry->d_name, "blue") && !have_blue &&
            append_path(candidate, sizeof(candidate), base, "brightness") &&
            path_is_file(candidate)) {
            strncpy(hw->blue_path, candidate, sizeof(hw->blue_path) - 1);
            hw->blue_path[sizeof(hw->blue_path) - 1] = '\0';
            have_blue = 1;
        }
    }
    closedir(dir);

    if (hw->has_multi || (have_red && have_green && have_blue)) {
        hw->kind = HW_SYSFS;
        if (hw->has_multi)
            le_log_info( "using sysfs LED backend (%s)", hw->multi_path);
        else
            le_log_info( "using sysfs individual RGB LED backend");
    } else {
        hw->multi_path[0] = '\0';
        hw->red_path[0] = '\0';
        hw->green_path[0] = '\0';
        hw->blue_path[0] = '\0';
        hw->brightness_path[0] = '\0';
    }
}

static int detect_i2c(void)
{
    static const unsigned int addresses[] = {
        0x32, 0x33, 0x34, 0x35, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65,
        0x66, 0x67
    };
    char path[64];
    size_t i;
    int bus;

    for (bus = 0; bus < 32; bus++) {
        int fd;
        int address_selected = 0;
        snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
            if (ioctl(fd, I2C_SLAVE, addresses[i]) == 0) {
                address_selected = 1;
                break;
            }
        }
        close(fd);
        if (address_selected) {
            le_log_info( "I2C bus %d is accessible", bus);
            le_log_info( "I2C LED driver not yet identified");
            return 1;
        }
    }
    return 0;
}

static void hardware_detect(struct hardware *hw, int force_stub)
{
    memset(hw, 0, sizeof(*hw));
    hw->kind = HW_STUB;
    if (force_stub) {
        le_log_warn( "LED stub backend forced by command line");
        return;
    }

    detect_sysfs(hw);
    if (hw->kind == HW_SYSFS)
        return;

    if (detect_i2c()) {
        hw->kind = HW_I2C_STUB;
        return;
    }
    le_log_warn( "no LED hardware found; using in-memory stub backend");
}

/* ----------------------------- JSON parser ------------------------------ */

static const char *json_skip_ws(const char *p)
{
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;
    return p;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static const char *json_string(const char *p, char *out, size_t out_size,
                               const char **endp)
{
    size_t used = 0;
    if (*p != '"')
        return NULL;
    p++;
    while (*p != '\0' && *p != '"') {
        unsigned int value = 0;
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            if (c == 'u') {
                int i, digit;
                for (i = 0; i < 4; i++) {
                    digit = hex_digit(*p++);
                    if (digit < 0)
                        return NULL;
                    value = (value << 4) | (unsigned int)digit;
                }
                c = value < 128U ? (char)value : '?';
            } else {
                switch (c) {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: return NULL;
                }
            }
        } else if ((unsigned char)c < 0x20U) {
            return NULL;
        }
        if (out != NULL && out_size > 1 && used + 1 < out_size)
            out[used] = c;
        used++;
    }
    if (*p != '"')
        return NULL;
    if (out != NULL && out_size > 0) {
        size_t at = used < out_size ? used : out_size - 1;
        out[at] = '\0';
    }
    p++;
    if (endp != NULL)
        *endp = p;
    return p;
}

static const char *json_skip_value(const char *p, unsigned int depth)
{
    char ignored[2];
    if (depth > 32)
        return NULL;
    p = json_skip_ws(p);
    if (*p == '"')
        return json_string(p, ignored, sizeof(ignored), NULL);
    if (*p == '{' || *p == '[') {
        char opening = *p++;
        char closing = opening == '{' ? '}' : ']';
        p = json_skip_ws(p);
        if (*p == closing)
            return p + 1;
        for (;;) {
            if (opening == '{') {
                if (*p != '"')
                    return NULL;
                p = json_string(p, ignored, sizeof(ignored), NULL);
                if (p == NULL)
                    return NULL;
                p = json_skip_ws(p);
                if (*p++ != ':')
                    return NULL;
            }
            p = json_skip_value(p, depth + 1);
            if (p == NULL)
                return NULL;
            p = json_skip_ws(p);
            if (*p == closing)
                return p + 1;
            if (*p++ != ',')
                return NULL;
            p = json_skip_ws(p);
        }
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        if (*p == '-')
            p++;
        if (*p == '0')
            p++;
        else {
            if (*p < '1' || *p > '9')
                return NULL;
            while (*p >= '0' && *p <= '9')
                p++;
        }
        if (*p == '.') {
            p++;
            if (*p < '0' || *p > '9')
                return NULL;
            while (*p >= '0' && *p <= '9')
                p++;
        }
        if (*p == 'e' || *p == 'E') {
            p++;
            if (*p == '+' || *p == '-')
                p++;
            if (*p < '0' || *p > '9')
                return NULL;
            while (*p >= '0' && *p <= '9')
                p++;
        }
        return p;
    }
    if (strncmp(p, "true", 4) == 0 || strncmp(p, "null", 4) == 0)
        return p + 4;
    if (strncmp(p, "false", 5) == 0)
        return p + 5;
    return NULL;
}

static int json_object_find(struct json_span object, const char *wanted,
                            struct json_span *value)
{
    const char *p = json_skip_ws(object.start);
    char key[64];
    if (p == NULL || *p++ != '{')
        return -1;
    p = json_skip_ws(p);
    if (*p == '}')
        return 0;
    for (;;) {
        const char *after_key;
        const char *start;
        const char *end;
        if (*p != '"')
            return -1;
        after_key = json_string(p, key, sizeof(key), &p);
        if (after_key == NULL)
            return -1;
        p = json_skip_ws(p);
        if (*p++ != ':')
            return -1;
        p = json_skip_ws(p);
        start = p;
        end = json_skip_value(p, 0);
        if (end == NULL)
            return -1;
        if (strcmp(key, wanted) == 0) {
            value->start = start;
            value->end = end;
            return 1;
        }
        p = json_skip_ws(end);
        if (*p == '}')
            return 0;
        if (*p++ != ',')
            return -1;
        p = json_skip_ws(p);
    }
}

static int json_span_is_object(struct json_span value)
{
    const char *p = json_skip_ws(value.start);
    return p < value.end && *p == '{';
}

static int json_get_string(struct json_span object, const char *name,
                           char *out, size_t out_size)
{
    struct json_span value;
    const char *end;
    int found = json_object_find(object, name, &value);
    if (found != 1)
        return -1;
    if (json_string(value.start, out, out_size, &end) == NULL ||
        json_skip_ws(end) != value.end)
        return -1;
    return 0;
}

static int json_get_unsigned(struct json_span object, const char *name,
                             unsigned long *out)
{
    struct json_span value;
    char number[64];
    size_t length;
    char *end;
    unsigned long result;
    int found = json_object_find(object, name, &value);
    if (found != 1)
        return -1;
    length = (size_t)(value.end - value.start);
    if (length == 0 || length >= sizeof(number))
        return -1;
    memcpy(number, value.start, length);
    number[length] = '\0';
    if (number[0] == '-')
        return -1;
    errno = 0;
    result = strtoul(number, &end, 10);
    if (errno == ERANGE || end == number || *end != '\0')
        return -1;
    *out = result;
    return 0;
}

static int parse_request(const char *text, struct request *request)
{
    struct json_span root, value;
    const char *end;
    unsigned long number;

    memset(request, 0, sizeof(*request));
    root.start = json_skip_ws(text);
    end = json_skip_value(root.start, 0);
    if (end == NULL || *json_skip_ws(end) != '\0')
        return -1;
    root.end = end;

    if (json_get_unsigned(root, "v", &number) != 0 || number != 1)
        return -1;
    if (json_get_unsigned(root, "id", &request->id) != 0)
        return -1;
    if (json_get_string(root, "cmd", request->command,
                        sizeof(request->command)) != 0)
        return -1;
    if (json_object_find(root, "args", &value) == 1) {
        if (!json_span_is_object(value))
            return -1;
        request->args = value;
        request->have_args = 1;
    }
    request->version = (int)number;
    return 0;
}

static int get_arg_unsigned(const struct request *request, const char *name,
                            unsigned int *out, unsigned int max)
{
    unsigned long value;
    if (!request->have_args ||
        json_get_unsigned(request->args, name, &value) != 0 || value > max)
        return -1;
    *out = (unsigned int)value;
    return 0;
}

static int get_arg_profile_field(const struct request *request,
                                  const char *field, int *profile)
{
    char name[32];
    int i;
    if (!request->have_args ||
        json_get_string(request->args, field, name, sizeof(name)) != 0)
        return -1;
    for (i = 0; i < PROFILE_COUNT; i++) {
        if (strcmp(name, profile_names[i]) == 0) {
            *profile = i;
            return 0;
        }
    }
    return -1;
}

static int get_arg_profile(const struct request *request, int *profile)
{
    return get_arg_profile_field(request, "name", profile);
}

/* ----------------------------- State storage ---------------------------- */

static void default_state(struct led_state *state)
{
    memset(state, 0, sizeof(*state));
    state->boot.r = 0;
    state->boot.g = 96;
    state->boot.b = 255;
    state->boot.brightness = 100;
    state->current = state->boot;

    state->profiles[PROFILE_LISTENING] = (struct colour){72, 185, 255, 100};
    state->profiles[PROFILE_THINKING] = (struct colour){168, 115, 239, 100};
    state->profiles[PROFILE_ERROR] = (struct colour){239, 80, 80, 100};
    state->profiles[PROFILE_DND] = (struct colour){190, 35, 35, 100};
}

static void read_colour(struct json_span object, struct colour *colour,
                        int require_brightness)
{
    unsigned int value;
    if (get_arg_unsigned(&(struct request){.args = object, .have_args = 1},
                         "r", &value, 255) == 0)
        colour->r = value;
    if (get_arg_unsigned(&(struct request){.args = object, .have_args = 1},
                         "g", &value, 255) == 0)
        colour->g = value;
    if (get_arg_unsigned(&(struct request){.args = object, .have_args = 1},
                         "b", &value, 255) == 0)
        colour->b = value;
    if (require_brightness &&
        get_arg_unsigned(&(struct request){.args = object, .have_args = 1},
                         "brightness", &value, 100) == 0)
        colour->brightness = value;
}

static void load_state(struct led_state *state)
{
    int fd;
    ssize_t n;
    char text[LE_ADAPTER_MSG_MAX + 1];
    struct json_span root, object;
    size_t i;

    fd = open(STATE_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0 || (size_t)n >= sizeof(text) - 1)
        return;
    text[n] = '\0';
    root.start = json_skip_ws(text);
    root.end = json_skip_value(root.start, 0);
    if (root.end == NULL || *json_skip_ws(root.end) != '\0' ||
        !json_span_is_object(root))
        return;

    if (json_object_find(root, "boot_profile", &object) == 1 &&
        json_span_is_object(object)) {
        read_colour(object, &state->boot, 1);
    }
    if (json_object_find(root, "current", &object) == 1 &&
        json_span_is_object(object)) {
        read_colour(object, &state->current, 1);
    }
    if (json_object_find(root, "profiles", &object) == 1 &&
        json_span_is_object(object)) {
        for (i = 0; i < PROFILE_COUNT; i++) {
            struct json_span profile;
            if (json_object_find(object, profile_names[i], &profile) == 1 &&
                json_span_is_object(profile)) {
                read_colour(profile, &state->profiles[i], 1);
            }
        }
    }
}

static int persist_state(const struct led_state *state)
{
    char text[4096];
    char temp_path[MAX_PATH];
    int n, fd;
    size_t i;

    n = snprintf(text, sizeof(text),
        "{\"current\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%u},"
        "\"boot_profile\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%u},"
        "\"profiles\":{",
        state->current.r, state->current.g, state->current.b,
        state->current.brightness, state->boot.r, state->boot.g, state->boot.b,
        state->boot.brightness);
    if (n < 0 || (size_t)n >= sizeof(text))
        return -1;
    for (i = 0; i < PROFILE_COUNT; i++) {
        int added = snprintf(text + n, sizeof(text) - (size_t)n,
            "%s\"%s\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%u}",
            i == 0 ? "" : ",", profile_names[i], state->profiles[i].r,
            state->profiles[i].g, state->profiles[i].b,
            state->profiles[i].brightness);
        if (added < 0 || (size_t)added >= sizeof(text) - (size_t)n)
            return -1;
        n += added;
    }
    if ((size_t)n + 3 >= sizeof(text))
        return -1;
    text[n++] = '}';
    text[n++] = '}';
    text[n] = '\0';

    (void)mkdir("/etc/libreecho", 0755);
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld", STATE_PATH,
             (long)getpid());
    fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        le_log_warn( "cannot persist LED state at %s: %s",
                    STATE_PATH, strerror(errno));
        return -1;
    }
    if (write_all_fd(fd, text, strlen(text)) != 0 || fsync(fd) != 0 ||
        close(fd) != 0 || rename(temp_path, STATE_PATH) != 0) {
        le_log_warn( "atomic LED state write failed: %s",
                    strerror(errno));
        close(fd);
        unlink(temp_path);
        return -1;
    }
    return 0;
}

/* --------------------------- Protocol and loop -------------------------- */

static int response_escape(char *out, size_t size, const char *text)
{
    size_t used = 0;
    while (*text != '\0' && used + 2 < size) {
        unsigned char c = (unsigned char)*text++;
        if (c == '"' || c == '\\') {
            if (used + 2 >= size)
                return -1;
            out[used++] = '\\';
            out[used++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            if (used + 2 >= size)
                return -1;
            out[used++] = '\\';
            out[used++] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
        } else if (c < 0x20U) {
            if (used + 6 >= size)
                return -1;
            used += (size_t)snprintf(out + used, size - used, "\\u%04x", c);
        } else {
            out[used++] = (char)c;
        }
    }
    if (*text != '\0' || used >= size)
        return -1;
    out[used] = '\0';
    return (int)used;
}

static int send_all(int fd, const char *buffer, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = send(fd, buffer + done, length - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            if (poll(&pfd, 1, 1000) <= 0)
                return -1;
            continue;
        }
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int send_response(int fd, unsigned long id, int ok, const char *data,
                         const char *error)
{
    char escaped[256];
    char response[LE_ADAPTER_MSG_MAX + 1];
    int n;

    if (ok) {
        n = snprintf(response, sizeof(response),
                     "{\"v\":1,\"id\":%lu,\"ok\":true,\"data\":%s}\n",
                     id, data != NULL ? data : "{}");
    } else {
        if (response_escape(escaped, sizeof(escaped),
                            error != NULL ? error : "request rejected") < 0)
            return -1;
        n = snprintf(response, sizeof(response),
                     "{\"v\":1,\"id\":%lu,\"ok\":false,\"error\":\"%s\"}\n",
                     id, escaped);
    }
    if (n < 0 || (size_t)n >= sizeof(response))
        return -1;
    return send_all(fd, response, (size_t)n);
}

static void apply_current(struct daemon_context *ctx)
{
    hardware_apply(&ctx->hw, &ctx->state.current);
}

static void apply_animated(struct daemon_context *ctx, double now)
{
    struct colour output = ctx->state.current;
    double phase = (now - ctx->animation_started) * 0.5;
    double wave;
    double multiplier;

    while (phase >= 1.0)
        phase -= 1.0;
    wave = 0.5 + 0.5 * sine_approx(phase * 6.28318530717958647692);
    multiplier = 0.30 + 0.70 * wave;
    output.brightness = (unsigned int)((double)ctx->state.current.brightness *
                                       multiplier + 0.5);
    hardware_apply(&ctx->hw, &output);
}

static void start_test(struct daemon_context *ctx, double now)
{
    if (!ctx->test_active) {
        ctx->test_saved = ctx->state.current;
        ctx->test_saved_animation = ctx->animation_active;
    }
    ctx->test_active = 1;
    ctx->test_started = now;
    ctx->animation_active = 0;
}

static void update_animation(struct daemon_context *ctx, double now)
{
    static const struct colour test_colours[] = {
        {255, 0, 0, 100}, {0, 255, 0, 100}, {0, 0, 255, 100},
        {255, 255, 255, 100}
    };
    double elapsed;

    if (ctx->test_active) {
        elapsed = now - ctx->test_started;
        if (elapsed >= 2.0) {
            ctx->test_active = 0;
            ctx->state.current = ctx->test_saved;
            ctx->animation_active = ctx->test_saved_animation;
            if (ctx->animation_active)
                ctx->animation_started = now;
            if (ctx->animation_active)
                apply_animated(ctx, now);
            else
                apply_current(ctx);
            return;
        }
        {
            unsigned int index = (unsigned int)(elapsed / 0.5);
            struct colour output = test_colours[index < 4 ? index : 3];
            output.brightness = ctx->test_saved.brightness;
            hardware_apply(&ctx->hw, &output);
        }
    } else if (ctx->animation_active) {
        apply_animated(ctx, now);
    }
}

static int status_json(const struct daemon_context *ctx, char *out,
                       size_t out_size)
{
    int n;
    size_t used = 0;
    size_t i;

    n = snprintf(out, out_size,
        "{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%u,"
        "\"boot_profile\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%u},"
        "\"profiles\":{",
        ctx->state.current.r, ctx->state.current.g, ctx->state.current.b,
        ctx->state.current.brightness, ctx->state.boot.r, ctx->state.boot.g,
        ctx->state.boot.b, ctx->state.boot.brightness);
    if (n < 0 || (size_t)n >= out_size)
        return -1;
    used = (size_t)n;
    for (i = 0; i < PROFILE_COUNT; i++) {
        n = snprintf(out + used, out_size - used,
            "%s\"%s\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%u}",
            i == 0 ? "" : ",", profile_names[i], ctx->state.profiles[i].r,
            ctx->state.profiles[i].g, ctx->state.profiles[i].b,
            ctx->state.profiles[i].brightness);
        if (n < 0 || (size_t)n >= out_size - used)
            return -1;
        used += (size_t)n;
    }
    if (used + 2 >= out_size)
        return -1;
    out[used++] = '}';
    out[used++] = '}';
    out[used] = '\0';
    return (int)used;
}

static int handle_request(struct daemon_context *ctx, int fd,
                          const char *line)
{
    struct request request;
    struct colour colour;
    char data[2048];
    unsigned int r, g, b, brightness;
    int profile;
    double now = monotonic_seconds();

    if (parse_request(line, &request) != 0)
        return send_response(fd, 0, 0, NULL, "malformed request");

    if (strcmp(request.command, "status") == 0) {
        if (status_json(ctx, data, sizeof(data)) < 0)
            return send_response(fd, request.id, 0, NULL, "status too large");
        return send_response(fd, request.id, 1, data, NULL);
    }

    if (strcmp(request.command, "set_colour") == 0) {
        if (get_arg_unsigned(&request, "r", &r, 255) != 0 ||
            get_arg_unsigned(&request, "g", &g, 255) != 0 ||
            get_arg_unsigned(&request, "b", &b, 255) != 0)
            return send_response(fd, request.id, 0, NULL,
                                 "set_colour requires r, g and b in 0..255");
        ctx->state.current.r = r;
        ctx->state.current.g = g;
        ctx->state.current.b = b;
        ctx->animation_active = 0;
        apply_current(ctx);
        persist_state(&ctx->state);
        return send_response(fd, request.id, 1, "{}", NULL);
    }

    if (strcmp(request.command, "set_brightness") == 0) {
        if (get_arg_unsigned(&request, "brightness", &brightness, 100) != 0)
            return send_response(fd, request.id, 0, NULL,
                                 "brightness must be in 0..100");
        ctx->state.current.brightness = brightness;
        ctx->animation_active = 0;
        apply_current(ctx);
        persist_state(&ctx->state);
        return send_response(fd, request.id, 1, "{}", NULL);
    }

    if (strcmp(request.command, "set_boot_profile") == 0) {
        colour = ctx->state.boot;
        if (get_arg_unsigned(&request, "r", &r, 255) != 0 ||
            get_arg_unsigned(&request, "g", &g, 255) != 0 ||
            get_arg_unsigned(&request, "b", &b, 255) != 0 ||
            get_arg_unsigned(&request, "brightness", &brightness, 100) != 0)
            return send_response(fd, request.id, 0, NULL,
                                 "set_boot_profile requires r, g, b and brightness");
        colour.r = r;
        colour.g = g;
        colour.b = b;
        colour.brightness = brightness;
        ctx->state.boot = colour;
        persist_state(&ctx->state);
        return send_response(fd, request.id, 1, "{}", NULL);
    }

    if (strcmp(request.command, "set_profile") == 0) {
        if (get_arg_profile(&request, &profile) != 0 ||
            get_arg_unsigned(&request, "r", &r, 255) != 0 ||
            get_arg_unsigned(&request, "g", &g, 255) != 0 ||
            get_arg_unsigned(&request, "b", &b, 255) != 0)
            return send_response(fd, request.id, 0, NULL,
                                 "set_profile requires a valid name and r, g, b");
        ctx->state.profiles[profile].r = r;
        ctx->state.profiles[profile].g = g;
        ctx->state.profiles[profile].b = b;
        ctx->state.profiles[profile].brightness = ctx->state.current.brightness;
        persist_state(&ctx->state);
        return send_response(fd, request.id, 1, "{}", NULL);
    }

    if (strcmp(request.command, "test") == 0) {
        start_test(ctx, now);
        update_animation(ctx, now);
        return send_response(fd, request.id, 1, "{}", NULL);
    }

    if (strcmp(request.command, "animate") == 0) {
        if (get_arg_profile_field(&request, "profile", &profile) != 0)
            return send_response(fd, request.id, 0, NULL,
                                 "animate requires a valid profile");
        ctx->state.current = ctx->state.profiles[profile];
        ctx->animation_profile = profile;
        ctx->animation_active = 1;
        ctx->animation_started = now;
        apply_animated(ctx, now);
        persist_state(&ctx->state);
        return send_response(fd, request.id, 1, "{}", NULL);
    }

    return send_response(fd, request.id, 0, NULL, "unknown command");
}

static void close_client(struct client *client)
{
    if (client->fd >= 0)
        close(client->fd);
    client->fd = -1;
    client->length = 0;
}

static void process_client_input(struct daemon_context *ctx,
                                 struct client *client)
{
    char *newline;
    size_t line_length;

    for (;;) {
        newline = memchr(client->input, '\n', client->length);
        if (newline == NULL)
            return;
        line_length = (size_t)(newline - client->input);
        if (line_length > 0 && client->input[line_length - 1] == '\r')
            line_length--;
        client->input[line_length] = '\0';
        if (handle_request(ctx, client->fd, client->input) != 0) {
            close_client(client);
            return;
        }
        line_length = (size_t)(newline - client->input) + 1;
        client->length -= line_length;
        memmove(client->input, client->input + line_length, client->length);
    }
}

static void receive_client(struct daemon_context *ctx, struct client *client)
{
    for (;;) {
        ssize_t n;
        if (client->length >= LE_ADAPTER_MSG_MAX) {
            send_response(client->fd, 0, 0, NULL, "message too large");
            close_client(client);
            return;
        }
        n = recv(client->fd, client->input + client->length,
                 LE_ADAPTER_MSG_MAX - client->length, 0);
        if (n > 0) {
            client->length += (size_t)n;
            process_client_input(ctx, client);
            if (client->fd < 0)
                return;
            continue;
        }
        if (n == 0) {
            close_client(client);
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        close_client(client);
        return;
    }
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static void accept_clients(struct daemon_context *ctx)
{
    for (;;) {
        int fd;
        int slot = -1;
        size_t i;
        fd = accept(ctx->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            le_log_warn( "accept failed: %s", strerror(errno));
            return;
        }
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (ctx->clients[i].fd < 0) {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0) {
            close(fd);
            continue;
        }
        if (set_nonblocking(fd) != 0) {
            close(fd);
            continue;
        }
        ctx->clients[slot].fd = fd;
        ctx->clients[slot].length = 0;
    }
}

static int mkdir_p(const char *path)
{
    char copy[MAX_PATH];
    char *p;
    size_t length;

    length = strlen(path);
    if (length == 0 || length >= sizeof(copy))
        return -1;
    memcpy(copy, path, length + 1);
    for (p = copy + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int make_listener(const char *path)
{
    struct sockaddr_un address;
    struct stat st;
    int fd;

    if (strlen(path) >= sizeof(address.sun_path)) {
        le_log_error( "socket path is too long");
        return -1;
    }
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            le_log_error( "refusing to unlink non-socket %s", path);
            return -1;
        }
        if (unlink(path) != 0) {
            le_log_error( "cannot unlink stale socket %s: %s", path,
                        strerror(errno));
            return -1;
        }
    } else if (errno != ENOENT) {
        le_log_error( "cannot inspect socket %s: %s", path,
                    strerror(errno));
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, MAX_CLIENTS) != 0 || chmod(path, 0660) != 0 ||
        set_nonblocking(fd) != 0) {
        le_log_error( "cannot create LED socket %s: %s", path,
                    strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

static int daemonize_process(void)
{
    pid_t pid;
    int null_fd;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);
    if (setsid() < 0)
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);
    umask(027);
    if (chdir("/") != 0)
        return -1;
    null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0)
        return -1;
    dup2(null_fd, STDIN_FILENO);
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO)
        close(null_fd);
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s [--socket PATH] [--foreground] [--stub]\n",
            program);
}

int main(int argc, char **argv)
{
    struct daemon_context ctx;
    struct sigaction action;
    int force_stub = 0;
    int i;

    memset(&ctx, 0, sizeof(ctx));
    ctx.listen_fd = -1;
    strncpy(ctx.socket_path, LE_ADAPTER_LED_SOCK,
            sizeof(ctx.socket_path) - 1);
    for (i = 0; i < MAX_CLIENTS; i++)
        ctx.clients[i].fd = -1;
    le_log_init("ledd", argc, argv);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--foreground") == 0) {
            ctx.foreground = 1;
        } else if (strcmp(argv[i], "--stub") == 0) {
            force_stub = 1;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            i++;
            if (strlen(argv[i]) >= sizeof(ctx.socket_path)) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            strncpy(ctx.socket_path, argv[i], sizeof(ctx.socket_path) - 1);
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "--debug") == 0 ||
                   strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "--syslog") == 0) {
            /* handled by le_log_init */
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (mkdir_p("/run/libreecho") != 0 && strcmp(ctx.socket_path,
                                                   LE_ADAPTER_LED_SOCK) == 0) {
        le_log_error( "cannot create /run/libreecho: %s",
                    strerror(errno));
        return EXIT_FAILURE;
    }
    if (strcmp(ctx.socket_path, LE_ADAPTER_LED_SOCK) != 0) {
        char parent[MAX_PATH];
        char *slash;
        strncpy(parent, ctx.socket_path, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        slash = strrchr(parent, '/');
        if (slash != NULL && slash != parent) {
            *slash = '\0';
            if (mkdir_p(parent) != 0) {
                le_log_error( "cannot create socket directory %s: %s",
                            parent, strerror(errno));
                return EXIT_FAILURE;
            }
        }
    }

    default_state(&ctx.state);
    load_state(&ctx.state);
    ctx.state.current = ctx.state.boot;
    hardware_detect(&ctx.hw, force_stub);
    hardware_apply(&ctx.hw, &ctx.state.current);

    if (!ctx.foreground && daemonize_process() != 0) {
        le_log_error( "daemonization failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    ctx.listen_fd = make_listener(ctx.socket_path);
    if (ctx.listen_fd < 0)
        return EXIT_FAILURE;

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    le_log_info( "LED daemon listening on %s", ctx.socket_path);
    while (!stop_requested) {
        struct pollfd fds[1 + MAX_CLIENTS];
        int slots[1 + MAX_CLIENTS];
        nfds_t nfds = 1;
        int timeout = -1;
        size_t j;

        fds[0].fd = ctx.listen_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        slots[0] = -1;
        if (ctx.test_active || ctx.animation_active)
            timeout = FRAME_MS;

        for (j = 0; j < MAX_CLIENTS; j++) {
            if (ctx.clients[j].fd >= 0) {
                fds[nfds].fd = ctx.clients[j].fd;
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                slots[nfds] = (int)j;
                nfds++;
            }
        }

        if (poll(fds, nfds, timeout) < 0) {
            if (errno == EINTR)
                continue;
            le_log_error( "poll failed: %s", strerror(errno));
            break;
        }
        if (fds[0].revents & POLLIN)
            accept_clients(&ctx);
        for (j = 1; j < nfds; j++) {
            int slot = slots[j];
            if (slot >= 0 && ctx.clients[slot].fd >= 0 &&
                (fds[j].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)))
                receive_client(&ctx, &ctx.clients[slot]);
        }
        if (ctx.test_active || ctx.animation_active)
            update_animation(&ctx, monotonic_seconds());
    }

    for (i = 0; i < MAX_CLIENTS; i++)
        close_client(&ctx.clients[i]);
    hardware_apply(&ctx.hw, &(struct colour){0, 0, 0, 0});
    if (ctx.listen_fd >= 0)
        close(ctx.listen_fd);
    unlink(ctx.socket_path);
    le_log_info( "LED daemon stopped");
    return EXIT_SUCCESS;
}
