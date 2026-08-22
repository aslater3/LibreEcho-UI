/*
 * LibreEcho audio companion daemon.
 *
 * This file deliberately uses the ALSA control UAPI directly.  There is no
 * dependency on libasound, which keeps the daemon usable on the target's
 * small musl userspace.  The amixer path is retained as a compatibility
 * fallback for vendor mixers whose controls cannot be written through the
 * normal control ioctls.
 */
#ifndef _DEFAULT_SOURCE
# define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
# define _POSIX_C_SOURCE 200809L
#endif

#include "adapter.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__has_include)
# if __has_include(<sound/asound.h>)
#  include <sound/asound.h>
#  define LE_HAVE_ASOUND_H 1
# endif
#elif defined(__linux__)
/* Linux target kernels normally ship the UAPI header. */
# include <sound/asound.h>
# define LE_HAVE_ASOUND_H 1
#endif

#ifndef LE_HAVE_ASOUND_H
/*
 * Minimal Linux 3.x control UAPI fallback.  These layouts match the 32-bit
 * ARM UAPI and are only used when sound/asound.h is not installed.
 */
# include <sys/ioctl.h>
# define SNDRV_CTL_ELEM_ID_NAME_MAXLEN 44
# define SNDRV_CTL_ELEM_TYPE_BOOLEAN 1
# define SNDRV_CTL_ELEM_TYPE_INTEGER 2
# define SNDRV_CTL_ELEM_TYPE_ENUMERATED 3
# define SNDRV_CTL_ELEM_TYPE_INTEGER64 6
# define SNDRV_CTL_ELEM_ACCESS_READ  (1U << 0)
# define SNDRV_CTL_ELEM_ACCESS_WRITE (1U << 1)
# define SNDRV_CTL_ELEM_IFACE_CARD 0
# define SNDRV_CTL_ELEM_IFACE_MIXER 2

typedef int snd_ctl_elem_type_t;
typedef int snd_ctl_elem_iface_t;

struct snd_ctl_card_info {
    int card;
    int pad;
    unsigned char id[16];
    unsigned char driver[16];
    unsigned char name[32];
    unsigned char longname[80];
    unsigned char reserved_[16];
    unsigned char mixername[80];
    unsigned char components[128];
};

struct snd_ctl_elem_id {
    unsigned int numid;
    snd_ctl_elem_iface_t iface;
    unsigned int device;
    unsigned int subdevice;
    unsigned char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
    unsigned int index;
};

struct snd_ctl_elem_list {
    unsigned int offset;
    unsigned int space;
    unsigned int used;
    unsigned int count;
    struct snd_ctl_elem_id *pids;
    unsigned char reserved[50];
};

struct snd_ctl_elem_info {
    struct snd_ctl_elem_id id;
    snd_ctl_elem_type_t type;
    unsigned int access;
    unsigned int count;
    int owner;
    union {
        struct { long min, max, step; } integer;
        struct { long long min, max, step; } integer64;
        unsigned char reserved[128];
    } value;
    unsigned char reserved[64];
};

struct snd_ctl_elem_value {
    struct snd_ctl_elem_id id;
    unsigned int indirect:1;
    union {
        union { long value[128]; long *value_ptr; } integer;
        union { long long value[64]; long long *value_ptr; } integer64;
        union { unsigned int item[128]; unsigned int *item_ptr; } enumerated;
        unsigned char bytes[512];
    } value;
    unsigned char reserved[128];
};

# define SNDRV_CTL_IOCTL_CARD_INFO \
    _IOR('U', 0x01, struct snd_ctl_card_info)
# define SNDRV_CTL_IOCTL_ELEM_LIST \
    _IOWR('U', 0x10, struct snd_ctl_elem_list)
# define SNDRV_CTL_IOCTL_ELEM_INFO \
    _IOWR('U', 0x11, struct snd_ctl_elem_info)
# define SNDRV_CTL_IOCTL_ELEM_READ \
    _IOWR('U', 0x12, struct snd_ctl_elem_value)
# define SNDRV_CTL_IOCTL_ELEM_WRITE \
    _IOWR('U', 0x13, struct snd_ctl_elem_value)
#endif

#define LE_MAX_CLIENTS 4
#define LE_CONTROL_MAX 1024
#define LE_DEFAULT_SOCKET LE_ADAPTER_AUDIO_SOCK
#define LE_PCM_RATE 48000
#define LE_TONE_SECONDS 2
#define LE_TONE_CHANNELS 2
#define LE_TONE_CHUNK_FRAMES 1024
#define LE_SYSTEM_AUDIO_BUS "/run/libreecho-audio/system.pcm"

/*
 * Wake acknowledgement chirp.
 *
 * Short and quiet on purpose.  It is an acknowledgement, not an alert: it
 * has to be audible across a room without being startling at night, and it
 * has to finish well before the speaker starts talking, because the same
 * audio is captured by the microphone and only the echo canceller keeps it
 * out of the transcript.  90ms of a rising two-tone pair is enough to be
 * recognised as "I heard you" and short enough not to overlap a command.
 */
#define LE_CHIRP_MS 90
#define LE_CHIRP_LOW_HZ 880.0
#define LE_CHIRP_HIGH_HZ 1320.0
#define LE_CHIRP_AMPLITUDE 4200.0

/*
 * Sleep-noise generator.
 *
 * Written to the media bus rather than the system bus, because this is
 * content the device is playing rather than a notification: it should duck
 * for an announcement and stop when something else takes over media, which
 * is what the media bus already gives us for free.
 *
 * Synthesis is deliberately trivial -- a PRNG plus a one-pole filter -- so
 * it costs no image space and no network.  The whole point of this feature
 * is that it keeps working with the internet down and no account.
 */
#define LE_MEDIA_AUDIO_BUS "/run/libreecho-audio/media.pcm"
#define LE_NOISE_WHITE 0
#define LE_NOISE_PINK 1
#define LE_NOISE_BROWN 2

static volatile sig_atomic_t g_stop;

struct audio_control {
    int found;
    int writable;
    int score;
    int mute_semantics;
    struct snd_ctl_elem_id id;
    struct snd_ctl_elem_info info;
    char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
};

struct audio_hw {
    int card;
    int ctl_fd;
    char ctl_path[64];
    char pcm_path[64];
    struct audio_control master;
    struct audio_control capture;
    struct audio_control capture_switch;
    struct audio_control amplifier_switch;
    int have_card_info;
    int output_available;
    int volume;
    int gain;
    int muted;
    int notification_volume;
    /* Last level explicitly asked for, by the API or the buttons; -1 until
       something asks.  Used to notice an outside writer changing the mixer. */
    int requested_volume;
    int volume_guard_warned;
    /* The running sleep-noise child, so a second start replaces the first
       rather than layering two generators onto the same bus. */
    pid_t noise_pid;
    int noise_colour;
    long noise_seconds;
    int startup_sound;
    int amplifier_on;
};

struct client {
    int fd;
    char input[LE_ADAPTER_MSG_MAX];
    size_t input_used;
    char output[LE_ADAPTER_MSG_MAX];
    size_t output_used;
    size_t output_sent;
};

static void signal_stop(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void signal_child(int signo)
{
    (void)signo;
}

static void reap_children(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    char *p;
    size_t n;

    if (!path || !*path)
        return -1;
    n = strlen(path);
    if (n >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, n + 1);

    for (p = tmp + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, mode) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int ensure_socket_parent(const char *socket_path)
{
    char parent[PATH_MAX];
    char *slash;
    size_t n;

    n = strlen(socket_path);
    if (n >= sizeof(parent))
        return -1;
    memcpy(parent, socket_path, n + 1);
    slash = strrchr(parent, '/');
    if (!slash)
        return 0;
    if (slash == parent)
        return 0;
    *slash = '\0';
    return mkdir_p(parent, 0755);
}

static int listen_socket(const char *path)
{
    struct sockaddr_un address;
    int fd;
    size_t path_len;

    if (ensure_socket_parent(path) < 0) {
        perror("audiod: create socket directory");
        return -1;
    }
    path_len = strlen(path);
    if (path_len >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (set_cloexec(fd) < 0 || set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, path_len + 1);
    (void)unlink(path);
    if (bind(fd, (struct sockaddr *)&address,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1)) < 0 ||
        listen(fd, LE_MAX_CLIENTS) < 0 || chmod(path, 0660) < 0) {
        int saved_errno = errno;
        close(fd);
        (void)unlink(path);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static void close_client(struct client *client)
{
    if (client->fd >= 0)
        close(client->fd);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static const char *json_value(const char *json, const char *key)
{
    char needle[96];
    const char *p;
    size_t key_len;

    key_len = strlen(key);
    if (key_len + 3 > sizeof(needle))
        return NULL;
    (void)snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = json;
    while ((p = strstr(p, needle)) != NULL) {
        const char *q = p + key_len + 2;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            ++q;
        if (*q == ':') {
            ++q;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
                ++q;
            return q;
        }
        p += key_len + 2;
    }
    return NULL;
}

static int json_long(const char *json, const char *key, long *value)
{
    const char *p = json_value(json, key);
    char *end;
    long parsed;

    if (!p)
        return -1;
    errno = 0;
    parsed = strtol(p, &end, 10);
    if (p == end || errno == ERANGE)
        return -1;
    if (*end != '\0' && *end != ',' && *end != '}' && *end != ']' &&
        *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')
        return -1;
    *value = parsed;
    return 0;
}

static int json_bool(const char *json, const char *key, int *value)
{
    const char *p = json_value(json, key);

    if (!p)
        return -1;
    if (!strncmp(p, "true", 4) &&
        (p[4] == '\0' || p[4] == ',' || p[4] == '}' || p[4] == ' ' ||
         p[4] == '\t' || p[4] == '\r' || p[4] == '\n')) {
        *value = 1;
        return 0;
    }
    if (!strncmp(p, "false", 5) &&
        (p[5] == '\0' || p[5] == ',' || p[5] == '}' || p[5] == ' ' ||
         p[5] == '\t' || p[5] == '\r' || p[5] == '\n')) {
        *value = 0;
        return 0;
    }
    return -1;
}

static int json_string(const char *json, const char *key,
                       char *out, size_t out_size)
{
    const char *p = json_value(json, key);
    size_t used = 0;

    if (!p || *p != '"' || out_size == 0)
        return -1;
    ++p;
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            if (!*p || used + 1 >= out_size)
                return -1;
            /* Command names do not need escaped characters; accept common JSON escapes. */
            switch (*p) {
            case '"': case '\\': case '/': out[used++] = *p; break;
            case 'n': out[used++] = '\n'; break;
            case 'r': out[used++] = '\r'; break;
            case 't': out[used++] = '\t'; break;
            default: return -1;
            }
        } else {
            if ((unsigned char)*p < 0x20 || used + 1 >= out_size)
                return -1;
            out[used++] = *p;
        }
        ++p;
        if (used + 1 >= out_size && *p != '"')
            return -1;
    }
    if (*p != '"')
        return -1;
    out[used] = '\0';
    return 0;
}

static int json_quote(char *output, size_t size, const char *input)
{
    size_t used = 0;

    while (input && *input) {
        unsigned char c = (unsigned char)*input++;

        if (c == '"' || c == '\\' || c == '\n' ||
            c == '\r' || c == '\t') {
            if (used + 2 >= size)
                return -1;
            output[used++] = '\\';
            output[used++] = c == '\n' ? 'n' :
                             c == '\r' ? 'r' :
                             c == '\t' ? 't' : (char)c;
        } else {
            if (c < 0x20 || used + 1 >= size)
                return -1;
            output[used++] = (char)c;
        }
    }
    output[used] = '\0';
    return 0;
}

static int parse_request(char *message, unsigned long *id,
                         char *command, size_t command_size)
{
    long version;
    long request_id;

    *id = 0;
    if (json_long(message, "v", &version) < 0 ||
        version != LE_ADAPTER_PROTO_VERSION ||
        json_long(message, "id", &request_id) < 0 || request_id < 0 ||
        json_string(message, "cmd", command, command_size) < 0)
        return -1;
    *id = (unsigned long)request_id;
    return 0;
}

static int response_ok(char *out, size_t out_size, unsigned long id,
                       const char *data)
{
    int n = snprintf(out, out_size,
                     "{\"v\":%d,\"id\":%lu,\"ok\":true,\"data\":%s}\n",
                     LE_ADAPTER_PROTO_VERSION, id, data);
    return n >= 0 && (size_t)n < out_size ? n : -1;
}

static int response_error(char *out, size_t out_size, unsigned long id,
                          const char *error)
{
    int n = snprintf(out, out_size,
                     "{\"v\":%d,\"id\":%lu,\"ok\":false,\"error\":\"%s\"}\n",
                     LE_ADAPTER_PROTO_VERSION, id, error);
    return n >= 0 && (size_t)n < out_size ? n : -1;
}

static void lower_name(const unsigned char *name, char *lower, size_t size)
{
    size_t i;
    if (size == 0)
        return;
    for (i = 0; i + 1 < size && name[i] != '\0'; ++i) {
        unsigned char ch = name[i];
        if (ch >= 'A' && ch <= 'Z')
            ch = (unsigned char)(ch - 'A' + 'a');
        lower[i] = (char)ch;
    }
    lower[i] = '\0';
}

static int contains(const char *name, const char *part)
{
    return strstr(name, part) != NULL;
}

static int control_score(const char *name, int kind, int *mute_semantics)
{
    int score = 0;
    int capture = contains(name, "capture") || contains(name, "mic") ||
                  contains(name, "input");
    int playback = contains(name, "playback") || contains(name, "speaker") ||
                   contains(name, "headphone");

    *mute_semantics = 0;
    if (kind == 0) { /* master playback volume */
        if (!contains(name, "volume") || capture)
            return 0;
        if (contains(name, "master") && contains(name, "playback")) score = 100;
        else if (contains(name, "speaker") && contains(name, "playback")) score = 90;
        else if (contains(name, "headphone") && contains(name, "playback")) score = 80;
        else if (playback) score = 70;
        else score = 20;
    } else if (kind == 1) { /* capture volume */
        if (!contains(name, "volume") || !capture)
            return 0;
        if (contains(name, "capture")) score = 100;
        else if (contains(name, "mic")) score = 90;
        else score = 50;
    } else { /* capture switch/mute */
        if ((!contains(name, "switch") && !contains(name, "mute")) || !capture)
            return 0;
        if (contains(name, "mute") && !contains(name, "switch")) {
            score = 80;
            *mute_semantics = 1;
        } else if (contains(name, "capture") && contains(name, "switch")) {
            score = 100;
        } else if (contains(name, "mic")) {
            score = 90;
        } else {
            score = 40;
        }
    }
    return score;
}

static void consider_control(struct audio_control *selected,
                             const struct snd_ctl_elem_id *id,
                             const struct snd_ctl_elem_info *info, int kind)
{
    char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
    int mute_semantics;
    int score;

    lower_name(id->name, name, sizeof(name));
    score = control_score(name, kind, &mute_semantics);
    if (score == 0 || score <= selected->score)
        return;
    memset(selected, 0, sizeof(*selected));
    selected->found = 1;
    selected->writable = (info->access & SNDRV_CTL_ELEM_ACCESS_WRITE) != 0;
    selected->score = score;
    selected->mute_semantics = mute_semantics;
    selected->id = *id;
    selected->info = *info;
    memcpy(selected->name, id->name, sizeof(selected->name));
    selected->name[sizeof(selected->name) - 1] = '\0';
}

static void select_control(struct audio_control *selected,
                           const struct snd_ctl_elem_id *id,
                           const struct snd_ctl_elem_info *info)
{
    memset(selected, 0, sizeof(*selected));
    selected->found = 1;
    selected->writable = (info->access & SNDRV_CTL_ELEM_ACCESS_WRITE) != 0;
    selected->id = *id;
    selected->info = *info;
    memcpy(selected->name, id->name, sizeof(selected->name));
    selected->name[sizeof(selected->name) - 1] = '\0';
}

static int enumerate_controls(struct audio_hw *audio)
{
    struct snd_ctl_elem_list list;
    struct snd_ctl_elem_id *ids;
    unsigned int count;
    unsigned int i;

    memset(&list, 0, sizeof(list));
    if (ioctl(audio->ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0)
        return -1;
    count = list.count;
    if (count == 0)
        return 0;
    if (count > LE_CONTROL_MAX)
        count = LE_CONTROL_MAX;

    ids = calloc(count, sizeof(*ids));
    if (!ids)
        return -1;
    memset(&list, 0, sizeof(list));
    list.space = count;
    list.pids = ids;
    if (ioctl(audio->ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
        free(ids);
        return -1;
    }

    for (i = 0; i < list.used && i < count; ++i) {
        struct snd_ctl_elem_info info;
        memset(&info, 0, sizeof(info));
        info.id = ids[i];
        if (ioctl(audio->ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
            continue;
        if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED &&
            !strcmp((const char *)ids[i].name, "Ext_Speaker_Amp_Switch")) {
            select_control(&audio->amplifier_switch, &ids[i], &info);
            continue;
        }
        if (info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN &&
            info.type != SNDRV_CTL_ELEM_TYPE_INTEGER &&
            info.type != SNDRV_CTL_ELEM_TYPE_INTEGER64)
            continue;
        consider_control(&audio->master, &ids[i], &info, 0);
        consider_control(&audio->capture, &ids[i], &info, 1);
        consider_control(&audio->capture_switch, &ids[i], &info, 2);
    }
    free(ids);
    return 0;
}

static int read_control_value(const struct audio_hw *audio,
                              const struct audio_control *control,
                              long long *value)
{
    struct snd_ctl_elem_value elem;

    if (!control->found || audio->ctl_fd < 0)
        return -1;
    memset(&elem, 0, sizeof(elem));
    elem.id = control->id;
    if (ioctl(audio->ctl_fd, SNDRV_CTL_IOCTL_ELEM_READ, &elem) < 0)
        return -1;
    if (control->info.type == SNDRV_CTL_ELEM_TYPE_INTEGER64)
        *value = elem.value.integer64.value[0];
    else if (control->info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN ||
             control->info.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
        *value = elem.value.integer.value[0];
    else if (control->info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED)
        *value = elem.value.enumerated.item[0];
    else
        return -1;
    return 0;
}

static int percent_from_range(long long value, long long min, long long max)
{
    long long result;
    if (max <= min)
        return 0;
    if (value <= min)
        return 0;
    if (value >= max)
        return 100;
    result = ((value - min) * 100 + (max - min) / 2) / (max - min);
    if (result < 0) result = 0;
    if (result > 100) result = 100;
    return (int)result;
}

static long long value_for_percent(long long min, long long max, int percent)
{
    long long range = max - min;
    return min + (range * percent + 50) / 100;
}

static int is_speaker_pcm_volume(const struct audio_control *control)
{
    return control->found && !strcmp(control->name, "PCM Playback Volume");
}

static long long speaker_value_for_percent(long long min, long long max,
                                           int percent)
{
    long long unity = min + 127;
    long long floor = min + 67;

    if (unity > max)
        unity = max;
    if (percent <= 0)
        return min;
    if (floor > unity)
        floor = min;
    return floor + ((unity - floor) * (percent - 1) + 49) / 99;
}

static int speaker_percent_from_value(long long value, long long min,
                                      long long max)
{
    long long unity = min + 127;
    long long floor = min + 67;

    if (unity > max)
        unity = max;
    if (value <= min)
        return 0;
    if (floor >= unity || value <= floor)
        return 1;
    if (value >= unity)
        return 100;
    return 1 + (int)(((value - floor) * 99 + (unity - floor) / 2) /
                     (unity - floor));
}

static int write_percent_control(const struct audio_hw *audio,
                                 const struct audio_control *control,
                                 int percent)
{
    struct snd_ctl_elem_value elem;
    long long min;
    long long max;
    long long value;
    unsigned int i;

    if (!control->found || !control->writable || audio->ctl_fd < 0)
        return -1;
    if (percent < 0 || percent > 100)
        return -1;
    if (control->info.type == SNDRV_CTL_ELEM_TYPE_INTEGER64) {
        min = control->info.value.integer64.min;
        max = control->info.value.integer64.max;
    } else if (control->info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
        min = control->info.value.integer.min;
        max = control->info.value.integer.max;
    } else {
        return -1;
    }
    if (max < min)
        return -1;
    value = is_speaker_pcm_volume(control)
          ? speaker_value_for_percent(min, max, percent)
          : value_for_percent(min, max, percent);

    memset(&elem, 0, sizeof(elem));
    elem.id = control->id;
    if (control->info.type == SNDRV_CTL_ELEM_TYPE_INTEGER64) {
        for (i = 0; i < control->info.count && i < 64; ++i)
            elem.value.integer64.value[i] = value;
    } else {
        for (i = 0; i < control->info.count && i < 128; ++i)
            elem.value.integer.value[i] = (long)value;
    }
    return ioctl(audio->ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &elem) < 0 ? -1 : 0;
}

static int write_switch_control(const struct audio_hw *audio,
                                const struct audio_control *control, int muted)
{
    struct snd_ctl_elem_value elem;
    long value;
    unsigned int i;

    if (!control->found || !control->writable || audio->ctl_fd < 0 ||
        control->info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN)
        return -1;
    value = control->mute_semantics ? muted : !muted;
    memset(&elem, 0, sizeof(elem));
    elem.id = control->id;
    for (i = 0; i < control->info.count && i < 128; ++i)
        elem.value.integer.value[i] = value;
    return ioctl(audio->ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &elem) < 0 ? -1 : 0;
}

static int run_amixer(const struct audio_hw *audio, const char *name,
                      const char *value)
{
    char card[16];
    char control[SNDRV_CTL_ELEM_ID_NAME_MAXLEN + 1];
    char setting[32];
    char *const argv[] = { (char *)"amixer", (char *)"-c", card,
                           (char *)"sset", control, setting, NULL };
    pid_t pid;
    pid_t waited;
    int status = 0;

    if (!name || !*name)
        return -1;
    (void)snprintf(card, sizeof(card), "%d", audio->card);
    (void)snprintf(control, sizeof(control), "%s", name);
    (void)snprintf(setting, sizeof(setting), "%s", value);
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp("amixer", argv);
        _exit(127);
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0)
        return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static const char *control_name_or_default(const struct audio_control *control,
                                           const char *fallback)
{
    return control->found && control->name[0] ? control->name : fallback;
}

/* Percent is not a round trip through the raw control, so ignore a point
   of rounding rather than reacting to it. */
#define VOLUME_GUARD_TOLERANCE 2

static int audio_set_volume(struct audio_hw *audio, int volume);

static void refresh_state(struct audio_hw *audio)
{
    long long value;

    if (read_control_value(audio, &audio->master, &value) == 0) {
        long long min = audio->master.info.type == SNDRV_CTL_ELEM_TYPE_INTEGER64
                      ? audio->master.info.value.integer64.min
                      : audio->master.info.value.integer.min;
        long long max = audio->master.info.type == SNDRV_CTL_ELEM_TYPE_INTEGER64
                      ? audio->master.info.value.integer64.max
                      : audio->master.info.value.integer.max;
        audio->volume = is_speaker_pcm_volume(&audio->master)
                      ? speaker_percent_from_value(value, min, max)
                      : percent_from_range(value, min, max);
        /*
         * Something outside audiod writes this mixer.  Any playback -- a
         * plain test tone is enough -- leaves the level at full, so a
         * volume set through the API or the buttons does not survive a
         * single spoken reply.  The writer has not been identified
         * (aslater3/LibreEcho#57), and until it is, hold the line: audiod
         * owns this control for the API and the physical buttons, so put
         * back the level that was actually asked for.
         *
         * This is a guard, not a fix.  It restores only a level someone
         * explicitly requested, so a device nobody has configured is left
         * alone, and it logs the first divergence so the underlying writer
         * stays visible instead of being silently papered over.
         */
        /*
         * A tolerance, because the percentage is not a round trip: the
         * control is a raw range and converting percent -> raw -> percent
         * can land a point away.  Without it the guard fired on that
         * rounding on every boot and, being one-shot, burned its warning on
         * an artefact and hid the real event.
         *
         * The raw control value is logged alongside the percentages.  The
         * percentage collapses the bottom of the range -- everything at or
         * below min+67 reads as 1% -- so the raw number is what identifies
         * an outside writer.
         */
        if (audio->requested_volume >= 0 &&
            abs(audio->volume - audio->requested_volume) >
                VOLUME_GUARD_TOLERANCE) {
            le_log_warn("audiod: volume changed underneath us: requested "
                        "%d%%, control now reads %d%% (raw %lld of %lld..%lld)"
                        "; restoring",
                        audio->requested_volume, audio->volume,
                        value, min, max);
            audio->volume_guard_warned = 1;
            if (audio_set_volume(audio, audio->requested_volume) == 0)
                audio->volume = audio->requested_volume;
        }
        audio->notification_volume = audio->volume;
    }
    if (read_control_value(audio, &audio->capture, &value) == 0) {
        long long min = audio->capture.info.type == SNDRV_CTL_ELEM_TYPE_INTEGER64
                      ? audio->capture.info.value.integer64.min
                      : audio->capture.info.value.integer.min;
        long long max = audio->capture.info.type == SNDRV_CTL_ELEM_TYPE_INTEGER64
                      ? audio->capture.info.value.integer64.max
                      : audio->capture.info.value.integer.max;
        audio->gain = percent_from_range(value, min, max);
    }
    if (read_control_value(audio, &audio->capture_switch, &value) == 0)
        audio->muted = audio->capture_switch.mute_semantics ? (value != 0) : (value == 0);
}

static int amplifier_active(const struct audio_hw *audio)
{
    char status_path[96];
    char status[64];
    int fd;
    ssize_t n;

    long long value;

    if (read_control_value(audio, &audio->amplifier_switch, &value) == 0)
        return value != 0;
    (void)snprintf(status_path, sizeof(status_path),
                   "/proc/asound/card%d/pcm23p/sub0/status", audio->card);
    fd = open(status_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = read(fd, status, sizeof(status) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    status[n] = '\0';
    return strstr(status, "closed") == NULL;
}

static int audio_set_volume(struct audio_hw *audio, int volume)
{
    char setting[16];
    if (volume < 0 || volume > 100)
        return -1;
    if (write_percent_control(audio, &audio->master, volume) < 0) {
        /* Never let a percentage-based fallback address the codec's
         * +24 dB region.  The direct control path above caps 100% at the
         * PCM control's 0 dB/unity value. */
        if (is_speaker_pcm_volume(&audio->master))
            return -1;
        (void)snprintf(setting, sizeof(setting), "%d%%", volume);
        if (run_amixer(audio,
                       control_name_or_default(&audio->master,
                                               "Master Playback Volume"),
                       setting) < 0)
            return -1;
    }
    audio->volume = volume;
    audio->notification_volume = volume;
    audio->requested_volume = volume;
    return 0;
}

static int audio_set_gain(struct audio_hw *audio, int gain)
{
    char setting[16];
    if (gain < 0 || gain > 100)
        return -1;
    if (write_percent_control(audio, &audio->capture, gain) < 0) {
        (void)snprintf(setting, sizeof(setting), "%d%%", gain);
        if (run_amixer(audio,
                       control_name_or_default(&audio->capture, "Capture Volume"),
                       setting) < 0)
            return -1;
    }
    audio->gain = gain;
    return 0;
}

static int audio_set_mute(struct audio_hw *audio, int muted)
{
    const char *setting = muted ? "nocap" : "cap";
    if (write_switch_control(audio, &audio->capture_switch, muted) < 0) {
        if (run_amixer(audio,
                       control_name_or_default(&audio->capture_switch,
                                               "Capture Switch"),
                       setting) < 0)
            return -1;
    }
    audio->muted = !!muted;
    return 0;
}

/*
 * Restore the owner's capture gain and playback volume at startup.
 *
 * audio_init() only ever *read* the mixer, so whatever the codec powered up
 * with became the reported state.  The values the web daemon persists were
 * therefore decorative: a gain set through the API survived in
 * web-config.json but the hardware reset to its own default on every boot.
 *
 * That is not merely cosmetic here.  The VAD floor has to sit above the
 * noise the capture path actually produces, so it is chosen for a given
 * gain; when the gain silently reverted, the configured floor no longer
 * matched the signal and the device either stopped hearing speech or
 * stopped detecting end-of-speech.  Restoring the gain keeps the two in
 * step across a reboot.
 *
 * The values arrive through the environment, which is how the init scripts
 * already pass persisted settings to the other daemons.
 */
static int audio_set_gain(struct audio_hw *audio, int gain);
static int audio_set_volume(struct audio_hw *audio, int volume);

static int level_from_environment(const char *name, int *value)
{
    const char *text = getenv(name);
    char *end;
    long parsed;

    if (!text || !text[0])
        return 0;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno || *end || parsed < 0 || parsed > 100)
        return 0;
    *value = (int)parsed;
    return 1;
}

static void apply_persisted_levels(struct audio_hw *audio)
{
    int value;

    if (!audio->have_card_info)
        return;
    if (level_from_environment("LE_AUDIO_MIC_GAIN", &value)) {
        if (audio_set_gain(audio, value) == 0)
            le_log_info("audiod: restored microphone gain %d", value);
        else
            le_log_warn("audiod: unable to restore microphone gain %d", value);
    }
    if (level_from_environment("LE_AUDIO_VOLUME", &value)) {
        if (audio_set_volume(audio, value) == 0)
            le_log_info("audiod: restored volume %d", value);
        else
            le_log_warn("audiod: unable to restore volume %d", value);
    }
}

static void audio_init(struct audio_hw *audio, int card)
{
    struct snd_ctl_card_info card_info;

    memset(audio, 0, sizeof(*audio));
    audio->card = card;
    audio->ctl_fd = -1;
    audio->volume = 50;
    audio->gain = 65;
    audio->requested_volume = -1;
    audio->volume_guard_warned = 0;
    audio->noise_pid = 0;
    audio->noise_colour = LE_NOISE_WHITE;
    audio->noise_seconds = 0;
    audio->muted = 0;
    audio->notification_volume = audio->volume;
    /* Boot playback is a hard-disabled image policy.  Keep the UI truthful
     * even when an older persistent config still requests a startup sound. */
    audio->startup_sound = 0;
    (void)snprintf(audio->ctl_path, sizeof(audio->ctl_path),
                   "/dev/snd/controlC%d", card);
    (void)snprintf(audio->pcm_path, sizeof(audio->pcm_path),
                   "/dev/snd/pcmC%dD23p", card);

    audio->ctl_fd = open(audio->ctl_path, O_RDWR | O_CLOEXEC);
    if (audio->ctl_fd < 0)
        audio->ctl_fd = open(audio->ctl_path, O_RDONLY | O_CLOEXEC);
    if (audio->ctl_fd < 0)
        return;

    memset(&card_info, 0, sizeof(card_info));
    if (ioctl(audio->ctl_fd, SNDRV_CTL_IOCTL_CARD_INFO, &card_info) < 0)
        return;
    audio->have_card_info = 1;
    (void)enumerate_controls(audio);
    refresh_state(audio);
    audio->output_available = audio->have_card_info && audio->master.found &&
                              access(audio->pcm_path, F_OK) == 0;
    audio->amplifier_on = 0;
    apply_persisted_levels(audio);
}

static void audio_destroy(struct audio_hw *audio)
{
    if (audio->ctl_fd >= 0)
        close(audio->ctl_fd);
    audio->ctl_fd = -1;
}

static void generate_tone(unsigned char *buffer, size_t frames,
                          double *sine, double *cosine)
{
    size_t i;
    const double step_sine = 0.0575640269595673; /* sin(2*pi*440/48000) */
    const double step_cosine = 0.9980267284282716; /* cos(2*pi*440/48000) */
    int16_t *samples = (int16_t *)buffer;

    for (i = 0; i < frames; ++i) {
        int16_t sample = (int16_t)(*sine * 12000.0);
        samples[i * 2] = sample;
        samples[i * 2 + 1] = sample;
        {
            double next_sine = *sine * step_cosine + *cosine * step_sine;
            double next_cosine = *cosine * step_cosine - *sine * step_sine;
            *sine = next_sine;
            *cosine = next_cosine;
        }
    }
}

static int write_tone_fd(int fd)
{
    unsigned char buffer[LE_TONE_CHUNK_FRAMES * LE_TONE_CHANNELS * sizeof(int16_t)];
    double sine = 0.0;
    double cosine = 1.0;
    int chunks = (LE_PCM_RATE * LE_TONE_SECONDS) / LE_TONE_CHUNK_FRAMES;
    int remainder = (LE_PCM_RATE * LE_TONE_SECONDS) % LE_TONE_CHUNK_FRAMES;
    int chunk;

    for (chunk = 0; chunk < chunks + (remainder != 0); ++chunk) {
        size_t frames = chunk < chunks ? LE_TONE_CHUNK_FRAMES : (size_t)remainder;
        size_t bytes = frames * LE_TONE_CHANNELS * sizeof(int16_t);
        size_t sent = 0;
        if (frames == 0)
            break;
        generate_tone(buffer, frames, &sine, &cosine);
        while (sent < bytes) {
            ssize_t n = write(fd, buffer + sent, bytes - sent);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd pfd = { fd, POLLOUT, 0 };
                int rc;

                do {
                    rc = poll(&pfd, 1, 1000);
                } while (rc < 0 && errno == EINTR);
                if (rc >= 0)
                    continue;
            }
            if (n <= 0)
                return -1;
            sent += (size_t)n;
        }
    }
    return 0;
}

static int write_chirp_fd(int fd)
{
    unsigned char buffer[LE_TONE_CHUNK_FRAMES * LE_TONE_CHANNELS *
                         sizeof(int16_t)];
    size_t total = (size_t)LE_PCM_RATE * LE_CHIRP_MS / 1000U;
    size_t done = 0;

    while (done < total) {
        size_t frames = total - done < LE_TONE_CHUNK_FRAMES
                      ? total - done : LE_TONE_CHUNK_FRAMES;
        int16_t *samples = (int16_t *)buffer;
        size_t bytes = frames * LE_TONE_CHANNELS * sizeof(int16_t);
        size_t sent = 0;
        size_t i;

        for (i = 0; i < frames; ++i) {
            size_t index = done + i;
            /* Second half steps up a fifth, which reads as a question
               being acknowledged rather than an error. */
            double hz = index * 2U < total ? LE_CHIRP_LOW_HZ
                                           : LE_CHIRP_HIGH_HZ;
            /* Ramp both ends so the bus does not get a click, which is
               louder and more startling than the tone itself. */
            double ramp = 1.0;
            size_t edge = total / 8U ? total / 8U : 1U;
            int16_t value;

            if (index < edge)
                ramp = (double)index / (double)edge;
            else if (index + edge > total)
                ramp = (double)(total - index) / (double)edge;
            value = (int16_t)(sin(2.0 * 3.14159265358979323846 * hz *
                                  (double)index / (double)LE_PCM_RATE) *
                              LE_CHIRP_AMPLITUDE * ramp);
            samples[i * 2] = value;
            samples[i * 2 + 1] = value;
        }
        while (sent < bytes) {
            ssize_t n = write(fd, buffer + sent, bytes - sent);

            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd pfd = { fd, POLLOUT, 0 };
                int rc;

                do {
                    rc = poll(&pfd, 1, 200);
                } while (rc < 0 && errno == EINTR);
                if (rc > 0)
                    continue;
            }
            if (n <= 0)
                return -1;
            sent += (size_t)n;
        }
        done += frames;
    }
    return 0;
}

static uint32_t noise_random(uint32_t *state)
{
    /* xorshift32: cheap, no libc dependency, and the spectral quality of
       the PRNG is far below what matters once it is filtered. */
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int write_noise_fd(int fd, int colour, double amplitude,
                          long seconds)
{
    unsigned char buffer[LE_TONE_CHUNK_FRAMES * LE_TONE_CHANNELS *
                         sizeof(int16_t)];
    uint32_t state = 0x1234567u;
    double brown = 0.0;
    double pink_a = 0.0, pink_b = 0.0, pink_c = 0.0;
    long frames_left = seconds > 0
        ? (long)LE_PCM_RATE * seconds : -1;

    for (;;) {
        size_t frames = LE_TONE_CHUNK_FRAMES;
        int16_t *samples = (int16_t *)buffer;
        size_t bytes;
        size_t sent = 0;
        size_t i;

        if (frames_left >= 0) {
            if (frames_left == 0)
                return 0;
            if ((long)frames > frames_left)
                frames = (size_t)frames_left;
        }
        bytes = frames * LE_TONE_CHANNELS * sizeof(int16_t);
        for (i = 0; i < frames; ++i) {
            /* -1..1 */
            double white = (double)(int32_t)noise_random(&state) /
                           2147483648.0;
            double value;

            if (colour == LE_NOISE_BROWN) {
                /* Integrate with a leak so it cannot wander to the rails. */
                brown = (brown + white * 0.05);
                if (brown > 1.0) brown = 1.0;
                if (brown < -1.0) brown = -1.0;
                brown *= 0.995;
                value = brown * 6.0;
            } else if (colour == LE_NOISE_PINK) {
                /* Three one-pole sections: a standard cheap approximation
                   of -3 dB per octave, close enough to sound right. */
                pink_a = 0.99765 * pink_a + white * 0.0990460;
                pink_b = 0.96300 * pink_b + white * 0.2965164;
                pink_c = 0.57000 * pink_c + white * 1.0526913;
                value = (pink_a + pink_b + pink_c + white * 0.1848) * 0.25;
            } else {
                value = white;
            }
            if (value > 1.0) value = 1.0;
            if (value < -1.0) value = -1.0;
            samples[i * 2] = (int16_t)(value * amplitude);
            samples[i * 2 + 1] = (int16_t)(value * amplitude);
        }
        while (sent < bytes) {
            ssize_t n = write(fd, buffer + sent, bytes - sent);

            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd pfd = { fd, POLLOUT, 0 };
                int rc;

                do {
                    rc = poll(&pfd, 1, 1000);
                } while (rc < 0 && errno == EINTR);
                if (rc > 0)
                    continue;
                if (rc == 0)
                    continue;
            }
            /* A closed reader means playback moved on; stop quietly. */
            if (n <= 0)
                return 0;
            sent += (size_t)n;
        }
        if (frames_left > 0)
            frames_left -= (long)frames;
    }
}

static void stop_noise(struct audio_hw *audio)
{
    if (audio->noise_pid > 0) {
        kill(audio->noise_pid, SIGTERM);
        while (waitpid(audio->noise_pid, NULL, 0) < 0 && errno == EINTR)
            ;
        audio->noise_pid = 0;
    }
}

static int start_noise(struct audio_hw *audio, int colour, int level,
                       long seconds)
{
    double amplitude;
    int fd;
    pid_t pid;

    if (!audio->output_available ||
        access(LE_MEDIA_AUDIO_BUS, F_OK) < 0)
        return -1;
    /* Replace rather than layer: two generators on one bus is noise in the
       unhelpful sense. */
    stop_noise(audio);
    if (level < 1)
        level = 1;
    if (level > 100)
        level = 100;
    /* Cap well below full scale.  This plays for hours next to someone
       asleep; headroom matters more than loudness, and the device volume
       is the control people will actually reach for. */
    amplitude = 8000.0 * (double)level / 100.0;
    fd = open(LE_MEDIA_AUDIO_BUS, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(fd);
        return -1;
    }
    if (pid == 0) {
        int result;

        signal(SIGTERM, SIG_DFL);
        result = write_noise_fd(fd, colour, amplitude, seconds);
        close(fd);
        _exit(result == 0 ? 0 : 1);
    }
    close(fd);
    audio->noise_pid = pid;
    audio->noise_colour = colour;
    audio->noise_seconds = seconds;
    return 0;
}

static int start_wake_chirp(const struct audio_hw *audio)
{
    int fd;
    pid_t pid;

    if (!audio->output_available ||
        access(LE_SYSTEM_AUDIO_BUS, F_OK) < 0)
        return -1;
    fd = open(LE_SYSTEM_AUDIO_BUS, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(fd);
        return -1;
    }
    if (pid == 0) {
        int result = write_chirp_fd(fd);
        close(fd);
        _exit(result == 0 ? 0 : 1);
    }
    close(fd);
    return 0;
}

static int start_test_tone(const struct audio_hw *audio)
{
    int fd;
    pid_t pid;

    if (!audio->output_available ||
        access(LE_SYSTEM_AUDIO_BUS, F_OK) < 0)
        return -1;
    fd = open(LE_SYSTEM_AUDIO_BUS, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(fd);
        return -1;
    }
    if (pid == 0) {
        int result = write_tone_fd(fd);
        close(fd);
        _exit(result == 0 ? 0 : 1);
    }
    close(fd);
    return 0;
}

static int queue_output(struct client *client, const char *message, size_t length)
{
    size_t pending = client->output_used - client->output_sent;
    if (length > sizeof(client->output) - pending)
        return -1;
    if (client->output_sent != 0 && pending != 0)
        memmove(client->output, client->output + client->output_sent, pending);
    client->output_sent = 0;
    memcpy(client->output + pending, message, length);
    client->output_used = pending + length;
    return 0;
}

static int handle_request(struct audio_hw *audio, char *message,
                          char *response, size_t response_size)
{
    char command[64];
    unsigned long id;
    long value;
    int boolean;
    char data[512];

    if (parse_request(message, &id, command, sizeof(command)) < 0)
        return response_error(response, response_size, 0, "malformed request");
    le_log_debug("audiod: cmd=\"%s\" id=%lu", command, id);

    if (!strcmp(command, "status")) {
        refresh_state(audio);
        audio->amplifier_on = amplifier_active(audio);
        (void)snprintf(data, sizeof(data),
                       "{\"volume\":%d,\"microphone_gain\":%d,"
                       "\"notification_volume\":%d,\"muted\":%s,"
                       "\"startup_sound\":%s,\"amplifier_on\":%s,"
                       "\"output_available\":%s}",
                       audio->volume, audio->gain, audio->notification_volume,
                       audio->muted ? "true" : "false",
                       audio->startup_sound ? "true" : "false",
                       audio->amplifier_on ? "true" : "false",
                       audio->output_available ? "true" : "false");
        return response_ok(response, response_size, id, data);
    }
    if (!strcmp(command, "set_volume")) {
        if (json_long(message, "volume", &value) < 0 || value < 0 || value > 100)
            return response_error(response, response_size, id, "volume must be 0-100");
        le_log_info("audiod: set_volume %d -> %d", audio->volume, (int)value);
        if (audio_set_volume(audio, (int)value) < 0)
            return response_error(response, response_size, id, "volume unavailable");
        return response_ok(response, response_size, id, "{}");
    }
    if (!strcmp(command, "set_gain")) {
        if (json_long(message, "gain", &value) < 0 || value < 0 || value > 100)
            return response_error(response, response_size, id, "gain must be 0-100");
        le_log_info("audiod: set_gain %d -> %d", audio->gain, (int)value);
        if (audio_set_gain(audio, (int)value) < 0)
            return response_error(response, response_size, id, "microphone gain unavailable");
        return response_ok(response, response_size, id, "{}");
    }
    if (!strcmp(command, "set_mute")) {
        if (json_bool(message, "muted", &boolean) < 0)
            return response_error(response, response_size, id, "muted must be boolean");
        if (audio_set_mute(audio, boolean) < 0)
            return response_error(response, response_size, id, "microphone mute unavailable");
        return response_ok(response, response_size, id, "{}");
    }
    if (!strcmp(command, "noise_start")) {
        char colour_name[16] = "white";
        long level = 40, minutes = 0;
        int colour;

        (void)json_string(message, "colour", colour_name, sizeof(colour_name));
        (void)json_long(message, "level", &level);
        (void)json_long(message, "minutes", &minutes);
        if (!strcmp(colour_name, "pink")) colour = LE_NOISE_PINK;
        else if (!strcmp(colour_name, "brown")) colour = LE_NOISE_BROWN;
        else if (!strcmp(colour_name, "white")) colour = LE_NOISE_WHITE;
        else return response_error(response, response_size, id,
                                   "colour must be white, pink or brown");
        if (level < 1 || level > 100 || minutes < 0 || minutes > 600)
            return response_error(response, response_size, id,
                                  "level must be 1-100 and minutes 0-600");
        if (start_noise(audio, colour, (int)level, minutes * 60) < 0)
            return response_error(response, response_size, id,
                                  "audio output unavailable");
        le_log_info("audiod: %s noise started (level %d, %s)",
                    colour_name, (int)level,
                    minutes ? "timed" : "until stopped");
        return response_ok(response, response_size, id, "{}");
    }
    if (!strcmp(command, "noise_stop")) {
        stop_noise(audio);
        le_log_info("audiod: noise stopped");
        return response_ok(response, response_size, id, "{}");
    }
    if (!strcmp(command, "wake_chirp")) {
        if (start_wake_chirp(audio) < 0)
            return response_error(response, response_size, id,
                                  "audio output unavailable");
        return response_ok(response, response_size, id, "{}");
    }
    if (!strcmp(command, "test_tone")) {
        le_log_info("audiod: test tone requested");
        if (start_test_tone(audio) < 0)
            return response_error(response, response_size, id, "audio output unavailable");
        return response_ok(response, response_size, id, "{\"playing\":true}");
    }
    if (!strcmp(command, "speak")) {
        char text[512];
        char escaped_text[1024];
        char request_id[64] = "";
        struct le_adapter *tts;
        char tts_response[LE_ADAPTER_MSG_MAX];
        int rc;

        if (json_string(message, "text", text, sizeof(text)) < 0)
            return response_error(response, response_size, id, "missing or invalid text field");
        if (text[0] == '\0')
            return response_error(response, response_size, id, "text must not be empty");
        if (json_quote(escaped_text, sizeof(escaped_text), text) < 0)
            return response_error(response, response_size, id,
                                  "text is too long");
        if (json_value(message, "request_id") &&
            json_string(message, "request_id", request_id,
                        sizeof(request_id)) < 0)
            return response_error(response, response_size, id,
                                  "invalid request_id");
        {
            const unsigned char *position =
                (const unsigned char *)request_id;

            while (*position &&
                   ((*position >= 'a' && *position <= 'z') ||
                    (*position >= 'A' && *position <= 'Z') ||
                    (*position >= '0' && *position <= '9') ||
                    *position == '-' || *position == '_'))
                ++position;
            if (*position)
                return response_error(response, response_size, id,
                                      "invalid request_id");
        }
        le_log_info("audiod: speak requested (%zu chars)", strlen(text));
        tts = le_adapter_connect(LE_ADAPTER_TTS_SOCK, 200);
        if (!tts)
            return response_error(response, response_size, id, "tts daemon unavailable");
        {
            char args[1200];

            if (request_id[0])
                (void)snprintf(
                    args, sizeof(args),
                    "{\"text\":\"%s\",\"request_id\":\"%s\"}",
                    escaped_text, request_id);
            else
                (void)snprintf(
                    args, sizeof(args),
                    "{\"text\":\"%s\"}", escaped_text);
            rc = le_adapter_call(tts, "speak", args, tts_response, sizeof(tts_response));
        }
        le_adapter_close(tts);
        if (rc != LE_ADAPTER_OK)
            return response_error(response, response_size, id, "tts speak failed");
        return response_ok(response, response_size, id, "{\"speaking\":true}");
    }
    if (!strcmp(command, "stop_speech")) {
        struct le_adapter *tts;
        int rc;

        le_log_info("audiod: stop speech requested");
        tts = le_adapter_connect(LE_ADAPTER_TTS_SOCK, 200);
        if (!tts)
            return response_error(response, response_size, id, "tts daemon unavailable");
        rc = le_adapter_call(tts, "stop_speech", NULL, NULL, 0);
        le_adapter_close(tts);
        if (rc != LE_ADAPTER_OK)
            return response_error(response, response_size, id, "tts stop failed");
        return response_ok(response, response_size, id, "{\"speaking\":false}");
    }
    return response_error(response, response_size, id, "unknown command");
}

static int process_client_input(struct audio_hw *audio, struct client *client)
{
    char response[LE_ADAPTER_MSG_MAX];
    char *newline;

    for (;;) {
        newline = memchr(client->input, '\n', client->input_used);
        if (!newline)
            break;
        {
            size_t line_length = (size_t)(newline - client->input);
            size_t consumed = line_length + 1;
            int response_length;
            if (line_length >= LE_ADAPTER_MSG_MAX)
                return -1;
            client->input[line_length] = '\0';
            response_length = handle_request(audio, client->input,
                                              response, sizeof(response));
            if (response_length < 0 ||
                queue_output(client, response, (size_t)response_length) < 0)
                return -1;
            memmove(client->input, client->input + consumed,
                    client->input_used - consumed);
            client->input_used -= consumed;
        }
    }
    if (client->input_used == sizeof(client->input)) {
        client->input_used = 0;
        return -1;
    }
    return 0;
}

static int read_client(struct audio_hw *audio, struct client *client)
{
    for (;;) {
        ssize_t n;
        if (client->input_used == sizeof(client->input))
            return -1;
        n = read(client->fd, client->input + client->input_used,
                 sizeof(client->input) - client->input_used);
        if (n > 0) {
            client->input_used += (size_t)n;
            if (process_client_input(audio, client) < 0)
                return -1;
            continue;
        }
        if (n == 0)
            return -1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

static int flush_client(struct client *client)
{
    while (client->output_sent < client->output_used) {
        ssize_t n = write(client->fd, client->output + client->output_sent,
                          client->output_used - client->output_sent);
        if (n > 0) {
            client->output_sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return -1;
    }
    client->output_used = 0;
    client->output_sent = 0;
    return 0;
}

static int accept_client(int listen_fd, struct client clients[LE_MAX_CLIENTS])
{
    int fd;
    int i;

    fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        return 0;
    (void)set_cloexec(fd);
    (void)set_nonblocking(fd);
    for (i = 0; i < LE_MAX_CLIENTS; ++i) {
        if (clients[i].fd < 0) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].fd = fd;
            return 1;
        }
    }
    close(fd);
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s [--socket PATH] [--card NUM] [--foreground] [--verbose] [--debug] [--quiet]\n",
            program);
}

int main(int argc, char **argv)
{
    const char *socket_path = LE_DEFAULT_SOCKET;
    int card = 0;
    int foreground = 0;
    int listen_fd;
    int i;
    struct client clients[LE_MAX_CLIENTS];
    struct audio_hw audio;
    struct sigaction action;
    struct sigaction child_action;

    memset(clients, 0, sizeof(clients));
    for (i = 0; i < LE_MAX_CLIENTS; ++i)
        clients[i].fd = -1;
    le_log_init("audiod", argc, argv);
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (!strcmp(argv[i], "--card") && i + 1 < argc) {
            char *end;
            long parsed = strtol(argv[++i], &end, 10);
            if (*argv[i] == '\0' || *end != '\0' || parsed < 0 || parsed > 31) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            card = (int)parsed;
        } else if (!strcmp(argv[i], "--foreground")) {
            foreground = 1;
        } else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "--debug") ||
                   !strcmp(argv[i], "--quiet") || !strcmp(argv[i], "--syslog")) {
            /* handled by le_log_init */
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    (void)foreground; /* daemonization is intentionally not implicit */
    le_log_info("audiod: starting (socket=%s, card=%d)", socket_path, card);

    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_stop;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    memset(&child_action, 0, sizeof(child_action));
    child_action.sa_handler = signal_child;
    child_action.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&child_action.sa_mask);
    (void)sigaction(SIGCHLD, &child_action, NULL);

    audio_init(&audio, card);
    listen_fd = listen_socket(socket_path);
    if (listen_fd < 0) {
        perror("audiod: listen socket");
        audio_destroy(&audio);
        return EXIT_FAILURE;
    }

    while (!g_stop) {
        struct pollfd pollfds[1 + LE_MAX_CLIENTS];
        int poll_to_client[1 + LE_MAX_CLIENTS];
        nfds_t nfds = 1;
        int poll_result;

        reap_children();

        pollfds[0].fd = listen_fd;
        pollfds[0].events = POLLIN;
        pollfds[0].revents = 0;
        poll_to_client[0] = -1;
        for (i = 0; i < LE_MAX_CLIENTS; ++i) {
            if (clients[i].fd < 0)
                continue;
            pollfds[nfds].fd = clients[i].fd;
            pollfds[nfds].events = POLLIN;
            if (clients[i].output_used > clients[i].output_sent)
                pollfds[nfds].events |= POLLOUT;
            pollfds[nfds].revents = 0;
            poll_to_client[nfds] = i;
            ++nfds;
        }

        poll_result = poll(pollfds, nfds, -1);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pollfds[0].revents & POLLIN)
            (void)accept_client(listen_fd, clients);
        for (i = 1; i < (int)nfds; ++i) {
            int client_index = poll_to_client[i];
            short revents = pollfds[i].revents;
            if (client_index < 0 || clients[client_index].fd < 0)
                continue;
            if (revents & (POLLERR | POLLNVAL | POLLHUP)) {
                close_client(&clients[client_index]);
                continue;
            }
            if ((revents & POLLIN) && read_client(&audio, &clients[client_index]) < 0) {
                close_client(&clients[client_index]);
                continue;
            }
            if ((revents & POLLOUT) && flush_client(&clients[client_index]) < 0)
                close_client(&clients[client_index]);
        }
    }

    for (i = 0; i < LE_MAX_CLIENTS; ++i)
        close_client(&clients[i]);
    close(listen_fd);
    (void)unlink(socket_path);
    audio_destroy(&audio);
    return EXIT_SUCCESS;
}
