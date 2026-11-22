#include "adapter.h"
#include "json.h"
#include "log.h"
#include "mic_beamformer.h"
#include "voice_aec.h"
#include "voice_dsp.h"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#include <errno.h>
#include <math.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MIC_CARD 0
#define MIC_DEVICE 24
#define MIC_RATE LE_VOICE_RATE
#define MIC_RAW_CHANNELS LE_VOICE_RAW_CHANNELS
#define MIC_CAPTURE_CHANNELS LE_VOICE_LOGICAL_CHANNELS
#define MIC_LOGICAL_CHANNELS LE_VOICE_LOGICAL_CHANNELS
#define MIC_BITS 24
#define MICCAL_UNITY LE_VOICE_MICCAL_UNITY
/*
 * The capture carries one speaker reference lane (the electrical playback
 * signal): it correlates with every microphone at the acoustic delay, which
 * lets a per-channel echo canceller work sample-synchronously inside the
 * same capture clock instead of chasing a buffered software mix.  Which raw
 * lane carries it comes from the model profile (capture.ref_lane); any
 * remaining lanes past it are undriven fillers.
 */
#define MIC_POST_AEC_FRAME_SAMPLES LE_VOICE_AEC_FRAME_SAMPLES
#define MIC_POST_AEC_DEFAULT_TAIL_MS 128
#define MIC_POST_AEC_MAX_TAIL_MS 400

struct micd_config {
    const char *socket_path;
    const char *idme_dir;
    const char *dt_idme_dir;
    const char *pcm_path;
    const char *capture_bin;
    const char *mixer_bin;
};

static volatile sig_atomic_t running = 1;

/*
 * Beam snapshot, shared between the stream worker that runs the beamformer
 * and the main process that answers "beams" requests.  The worker is a
 * forked child, so this lives in an anonymous shared mapping made before any
 * fork.  seq is odd while the worker is writing (a seqlock); readers retry.
 */
struct beam_view {
    volatile unsigned int seq;
    volatile int running;
    unsigned int beam_count;
    int azimuth_deg[LE_BEAM_MAX_BEAMS];
    float power[LE_BEAM_MAX_BEAMS];
    float floor[LE_BEAM_MAX_BEAMS];
    int selected_deg;
    int locked;
    unsigned long hops;
    unsigned long switches;
    long long updated_ms;
    /* enum le_mic_mix, set by "set_mix" and read by the stream worker per hop */
    volatile unsigned int mix_mode;
    /* Array lane used by the "single" mode; -1 is automatic (lowest selected lane). */
    volatile int mix_lane;
    /* Runtime gate for the per-channel static FIR echo subtraction. */
    volatile int fir_enable;
    /* Set by the worker when the measured IR (and thus anti-beam) loaded. */
    volatile int antibeam_available;
};

static struct beam_view *beam_view;

static unsigned int current_mix_mode(void)
{
    return beam_view ? beam_view->mix_mode : LE_MIC_MIX_BEAMFORMER;
}

static int current_mix_lane(void)
{
    return beam_view ? beam_view->mix_lane : -1;
}

/*
 * The mode named by "key" in a JSON object: "beamformer", "average" or
 * "single".  Returns -1 when the key is absent or the value is unknown.
 */
static int mix_mode_from_json(const char *text, const char *key)
{
    char name[24];
    const char *p;
    size_t length = 0;

    if (!text || !key)
        return -1;
    if (!(p = strstr(text, key)))
        return -1;
    p += strlen(key);
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p++ != ':')
        return -1;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p++ != '"')
        return -1;
    while (p[length] && p[length] != '"' && length < sizeof(name) - 1) {
        name[length] = p[length];
        ++length;
    }
    if (p[length] != '"')
        return -1;
    name[length] = '\0';
    return le_mic_mix_parse(name);
}

/* The owner's saved mix settings from LE_MIC_CONFIG are read once at start-up. */
#define MIX_CONFIG_MAX_BYTES 16384

/*
 * The integer named by "key" in a JSON object, which must be within
 * [-1, MIC_LOGICAL_CHANNELS - 1]; -1 is the automatic lane.  Returns -1 when
 * the key is absent or the value is out of range, otherwise 0 with *lane set.
 */
static int lane_from_json(const char *text, const char *key, int *lane)
{
    const char *p;
    char *end;
    long value;

    if (!text || !key || !lane || !(p = strstr(text, key)))
        return -1;
    p += strlen(key);
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p++ != ':')
        return -1;
    while (*p == ' ' || *p == '\t')
        ++p;
    errno = 0;
    value = strtol(p, &end, 10);
    if (errno || end == p || value < -1 || value >= MIC_LOGICAL_CHANNELS)
        return -1;
    *lane = (int)value;
    return 0;
}

/* The owner's saved mix mode and lane from LE_MIC_CONFIG, read once at start-up. */
static void saved_mix_settings(int *mode, int *lane)
{
    const char *path = getenv("LE_MIC_CONFIG");
    static char text[MIX_CONFIG_MAX_BYTES];
    size_t used = 0;
    FILE *file;
    int value;

    *mode = LE_MIC_MIX_BEAMFORMER;
    *lane = -1;
    if (path && path[0] && (file = fopen(path, "r"))) {
        used = fread(text, 1, sizeof(text) - 1, file);
        (void)fclose(file);
    }
    text[used] = '\0';
    value = mix_mode_from_json(text, "\"microphone_mix\"");
    if (value >= 0)
        *mode = (int)value;
    if (lane_from_json(text, "\"microphone_lane\"", &value) == 0)
        *lane = value;
}

static long long monotonic_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void beam_view_publish(const struct le_beamformer *bf)
{
    unsigned int b;

    if (!beam_view)
        return;
    beam_view->seq++;
    __sync_synchronize();
    beam_view->running = 1;
    beam_view->beam_count = bf->beam_count;
    for (b = 0; b < bf->beam_count && b < LE_BEAM_MAX_BEAMS; ++b) {
        beam_view->azimuth_deg[b] = bf->azimuth_deg[b];
        beam_view->power[b] = bf->power[b];
        beam_view->floor[b] = bf->floor[b];
    }
    beam_view->selected_deg = le_beamformer_selected_azimuth(bf);
    beam_view->locked = bf->locked >= 0;
    beam_view->hops++;
    beam_view->switches = bf->switches;
    beam_view->updated_ms = monotonic_ms();
    __sync_synchronize();
    beam_view->seq++;
}

static void beam_view_stop(void)
{
    if (!beam_view)
        return;
    beam_view->seq++;
    __sync_synchronize();
    beam_view->running = 0;
    __sync_synchronize();
    beam_view->seq++;
}

/* Hann-windowed full-scale tone, summed over the scoring band: 0 dB. */
#define BEAM_FULL_SCALE_DB 126.4

static double beam_db(float power)
{
    return 10.0 * log10((double)power + 1.0) - BEAM_FULL_SCALE_DB;
}

/*
 * Rough direction of arrival: the strongest beam, refined by fitting a
 * parabola through it and its two neighbours (beams wrap around).  The
 * contrast is how far the peak stands above the average beam, which says
 * how directional the sound field is; diffuse noise gives close to 0 dB.
 */
static int format_beams(char *buffer, size_t size)
{
    struct beam_view v;
    unsigned int seq, b, peak = 0, tries;
    double db[LE_BEAM_MAX_BEAMS], mean = 0.0, doa, offset = 0.0;
    size_t used;
    int n;

    if (!beam_view)
        return -1;
    for (tries = 0; tries < 100; ++tries) {
        seq = beam_view->seq;
        __sync_synchronize();
        memcpy(&v, (const void *)beam_view, sizeof(v));
        __sync_synchronize();
        if (!(seq & 1U) && seq == beam_view->seq)
            break;
    }
    if (tries == 100)
        return -1;
    if (!v.running || v.beam_count == 0 || v.beam_count > LE_BEAM_MAX_BEAMS) {
        n = snprintf(buffer, size,
                     "{\"running\":false,\"reason\":\"no beamformed stream is "
                     "running; the wake word service starts one\"}");
        return n < 0 || (size_t)n >= size ? -1 : n;
    }
    for (b = 0; b < v.beam_count; ++b) {
        db[b] = beam_db(v.power[b]);
        mean += db[b];
        if (db[b] > db[peak])
            peak = b;
    }
    mean /= v.beam_count;
    {
        double l = db[(peak + v.beam_count - 1) % v.beam_count];
        double c = db[peak], r = db[(peak + 1) % v.beam_count];
        double curve = l - 2.0 * c + r;
        if (curve < -1e-9)
            offset = 0.5 * (l - r) / curve;
        if (offset > 0.5) offset = 0.5;
        if (offset < -0.5) offset = -0.5;
    }
    doa = v.azimuth_deg[peak] + offset * (360.0 / v.beam_count);
    while (doa < 0.0) doa += 360.0;
    while (doa >= 360.0) doa -= 360.0;
    n = snprintf(buffer, size,
                 "{\"running\":true,\"age_ms\":%lld,\"hops\":%lu,"
                 "\"hop_ms\":8,\"switches\":%lu,"
                 "\"reference\":\"0 deg front (volume down), clockwise viewed "
                 "from above\",\"selected_deg\":%d,\"locked\":%s,"
                 "\"doa_deg\":%.1f,\"peak_db\":%.1f,\"contrast_db\":%.1f,"
                 "\"beams\":[",
                 monotonic_ms() - v.updated_ms, v.hops, v.switches,
                 v.selected_deg, v.locked ? "true" : "false",
                 doa, db[peak], db[peak] - mean);
    if (n < 0 || (size_t)n >= size)
        return -1;
    used = (size_t)n;
    for (b = 0; b < v.beam_count; ++b) {
        n = snprintf(buffer + used, size - used,
                     "%s{\"deg\":%d,\"power_db\":%.1f,\"snr_db\":%.1f}",
                     b ? "," : "", v.azimuth_deg[b], db[b],
                     10.0 * log10(((double)v.power[b] + 1.0) /
                                  ((double)v.floor[b] + 1.0)));
        if (n < 0 || (size_t)n >= size - used)
            return -1;
        used += (size_t)n;
    }
    n = snprintf(buffer + used, size - used, "]}");
    if (n < 0 || (size_t)n >= size - used)
        return -1;
    return (int)(used + (size_t)n);
}
static volatile sig_atomic_t child_changed = 0;

static void stop_handler(int signo)
{
    (void)signo;
    running = 0;
}

static void child_handler(int signo)
{
    (void)signo;
    child_changed = 1;
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const char *p = buffer;

    while (size > 0) {
        ssize_t written = send(fd, p, size, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        p += written;
        size -= (size_t)written;
    }
    return 0;
}


static int read_request_line(int fd, char *buffer, size_t size)
{
    size_t used = 0;

    while (used + 1 < size) {
        struct pollfd descriptor;
        ssize_t count;

        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 3000) <= 0)
            return -1;
        count = read(fd, buffer + used, 1);
        if (count <= 0)
            return -1;
        if (buffer[used++] == '\n') {
            buffer[used - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

static int read_miccal_file(const char *path, int *value)
{
    FILE *file;

    file = fopen(path, "r");
    if (!file)
        return 0;
    if (fscanf(file, "%d", value) != 1 || *value < 0 || *value > 65535) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

static int read_miccal_list(const char *path, int values[7])
{
    FILE *file;
    int count = 0;
    int i;

    file = fopen(path, "r");
    if (!file)
        return 0;
    for (i = 0; i < MIC_LOGICAL_CHANNELS; ++i) {
        int value;

        if (fscanf(file, "%d", &value) != 1 ||
            value < 0 || value > 65535)
            break;
        values[i] = value;
        ++count;
    }
    fclose(file);
    return count;
}

static int read_miccal(const char *idme_dir, const char *dt_idme_dir,
                       int values[7])
{
    char path[256];
    int count = 0;
    int i;

    for (i = 0; i < MIC_LOGICAL_CHANNELS; ++i) {
        int value;

        values[i] = MICCAL_UNITY;
        if (snprintf(path, sizeof(path), "%s/miccal.%d",
                     idme_dir, i) >= (int)sizeof(path))
            continue;
        if (read_miccal_file(path, &value)) {
            values[i] = value;
            ++count;
        }
    }
    if (count != 0)
        return count;

    if (snprintf(path, sizeof(path), "%s/miccal",
                 idme_dir) >= (int)sizeof(path))
        return 0;
    {
        count = read_miccal_list(path, values);
    }
    if (count != 0)
        return count;

    for (i = 0; i < MIC_LOGICAL_CHANNELS; ++i) {
        int value;

        if (snprintf(path, sizeof(path), "%s/miccal.%d/value",
                     dt_idme_dir, i) >= (int)sizeof(path))
            continue;
        if (read_miccal_file(path, &value)) {
            values[i] = value;
            ++count;
        }
    }
    return count;
}

static const char *miccal_source(const char *idme_dir,
                                 const char *dt_idme_dir)
{
    static char source[256];
    char path[256];
    int i;

    for (i = 0; i < MIC_LOGICAL_CHANNELS; ++i) {
        if (snprintf(path, sizeof(path), "%s/miccal.%d",
                     idme_dir, i) < (int)sizeof(path) &&
            access(path, R_OK) == 0) {
            snprintf(source, sizeof(source), "%s/miccal.N", idme_dir);
            return source;
        }
    }
    if (snprintf(path, sizeof(path), "%s/miccal", idme_dir) <
            (int)sizeof(path) && access(path, R_OK) == 0) {
        snprintf(source, sizeof(source), "%s/miccal", idme_dir);
        return source;
    }
    for (i = 0; i < MIC_LOGICAL_CHANNELS; ++i) {
        if (snprintf(path, sizeof(path), "%s/miccal.%d/value",
                     dt_idme_dir, i) < (int)sizeof(path) &&
            access(path, R_OK) == 0) {
            snprintf(source, sizeof(source), "%s/miccal.N/value",
                     dt_idme_dir);
            return source;
        }
    }
    return "unavailable";
}

struct mixer_setting {
    const char *control;
    const char *value;
    const char *value2;
};

static int apply_mixer_setting(const char *mixer_bin,
                               const struct mixer_setting *setting)
{
    pid_t child;
    int status;

    child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        char *const argv[] = {
            (char *)"tinymix", (char *)"set",
            (char *)setting->control, (char *)setting->value,
            (char *)setting->value2, NULL
        };
        int null_fd = open("/dev/null", O_WRONLY);

        if (null_fd >= 0) {
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        execv(mixer_bin, argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/*
 * Post-mixdown digital gain for the calibrated mono stream.
 *
 * The analogue PGA cannot reach the level the wake model needs.  Scoring
 * the device's own capture through the openwakeword alexa_v0.1 model, the
 * stream peaks at 0.41 against a 0.45 accept gate with one supporting
 * frame where two are required -- a near miss on every attempt.  The same
 * audio amplified scores 0.70 at three times and 0.80 at six, both of
 * which fire.  Raising the PGA further does not work: measured on
 * hardware, MICPGA 110 is about seven times quieter than 80, so the
 * control is not monotonic across its nominal range.
 *
 * Applying the gain here instead reproduces exactly what was measured,
 * and it lands on both consumers of this stream -- the wake detector and
 * speech recognition, which was transcribing single characters from the
 * same too-quiet audio.
 *
 * Digital gain does not improve signal to noise; it scales both.  That is
 * fine for a level-sensitive model, but it means any VAD floor configured
 * against the old level has to scale with it, or end-of-speech detection
 * breaks in the way it did before.
 */
#define MIC_DIGITAL_GAIN_UNITY 100U
#define MIC_DIGITAL_GAIN_DEFAULT 400U
#define MIC_DIGITAL_GAIN_MAX 1600U

static unsigned int digital_gain_from_environment(void)
{
    const char *text = getenv("LE_MIC_DIGITAL_GAIN");
    char *end;
    unsigned long parsed;

    if (!text || !text[0])
        return MIC_DIGITAL_GAIN_DEFAULT;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed < MIC_DIGITAL_GAIN_UNITY ||
        parsed > MIC_DIGITAL_GAIN_MAX)
        return MIC_DIGITAL_GAIN_DEFAULT;
    return (unsigned int)parsed;
}

static int mic_ir_ntaps;

static int read_device_type_id(const char *dt_idme_dir, char *out,
                               size_t size);

/* Per-model profile: models/<device_type_id>.json wires up the shipped
 * IR file and the mic geometry for hardware whose layout differs from
 * the compiled-in Echo 2 ring. Missing file or fields keep defaults. */
struct mic_model {
    char ir_file[64];
    double mic_x_um[LE_VOICE_LOGICAL_CHANNELS];
    double mic_y_um[LE_VOICE_LOGICAL_CHANNELS];
    unsigned int mic_pos_count;
    unsigned int ref_lane;
    unsigned int beam_mic_mask;
};

static unsigned int json_floats(const char *p, size_t n, double *out,
                                unsigned int cap)
{
    unsigned int count = 0;
    const char *end = p + n;

    while (p < end && count < cap) {
        char *stop;
        double v;
        while (p < end && (isspace((unsigned char)*p) || *p == ',' ||
                           *p == '[' || *p == ']'))
            ++p;
        v = strtod(p, &stop);
        if (stop == p)
            break;
        out[count++] = v;
        p = stop;
    }
    return count;
}

static void mic_model_load(const char *dt_idme_dir, struct mic_model *model)
{
    const char *dir = getenv("LE_MIC_MODEL_DIR");
    char path[320], id[64], *json = NULL;
    FILE *f;
    long size;

    memset(model, 0, sizeof(*model));
    /* Convention: the speaker reference is the last capture lane before
     * any filler lanes; capture.ref_lane in the model profile overrides. */
    model->ref_lane = LE_VOICE_RAW_CHANNELS - 2;
    if (!dir)
        dir = "/usr/local/share/libreecho/models";
    if (read_device_type_id(dt_idme_dir, id, sizeof(id)) < 0 ||
        snprintf(path, sizeof(path), "%s/%s.json", dir, id) >=
            (int)sizeof(path))
        return;
    f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size < 16384 && (json = malloc((size_t)size + 1))) {
        if (fread(json, 1, (size_t)size, f) == (size_t)size) {
            const char *arr;
            size_t arr_n;

            json[size] = '\0';
            {
                int lane;

                if (json_get_int(json, "ref_lane", &lane) > 0 &&
                    lane >= 0 && lane < LE_VOICE_RAW_CHANNELS)
                    model->ref_lane = (unsigned int)lane;
            }
            (void)json_get_string(json, "ir_file", model->ir_file,
                                  sizeof(model->ir_file));
            if (json_get_array_top_level(json, "beam_mics_default", &arr,
                                         &arr_n) > 0) {
                double lanes[LE_VOICE_LOGICAL_CHANNELS];
                unsigned int i, n = json_floats(arr, arr_n, lanes,
                                                LE_VOICE_LOGICAL_CHANNELS);
                for (i = 0; i < n; ++i) {
                    unsigned int lane = (unsigned int)lanes[i];
                    if (lane < LE_VOICE_LOGICAL_CHANNELS)
                        model->beam_mic_mask |= 1U << lane;
                }
            }
            if (json_get_array_top_level(json, "mic_x_um", &arr, &arr_n) > 0) {
                unsigned int nx = json_floats(arr, arr_n, model->mic_x_um,
                                              LE_VOICE_LOGICAL_CHANNELS);
                if (json_get_array_top_level(json, "mic_y_um", &arr,
                                             &arr_n) > 0 &&
                    json_floats(arr, arr_n, model->mic_y_um,
                                LE_VOICE_LOGICAL_CHANNELS) == nx)
                    model->mic_pos_count = nx;
            }
            le_log_info("mic model: %s (ir=%s, positions=%u)", path,
                        model->ir_file[0] ? model->ir_file : "none",
                        model->mic_pos_count);
            le_log_info("mic model: ref_lane=%u", model->ref_lane);
        }
        free(json);
    }
    fclose(f);
}

/*
 * Microphones fed to the fixed beams.  The default is the stock speech set
 * (lanes 0, 1, 3 and 4); LE_MIC_BEAM_MICS="all" or a list such as "0,1,3,4,6"
 * selects another subset for comparison.  An invalid value falls back to the
 * default rather than failing the stream.
 */
static unsigned int beam_mic_mask(const struct mic_model *model)
{
    unsigned int mask = le_beamformer_parse_mics(getenv("LE_MIC_BEAM_MICS"));

    /* LE_MIC_BEAM_MICS stays the escape hatch for experiments; next a
     * beam_mics_default list from the model profile. */
    if (mask && (mask & (mask - 1U)))
        return mask;
    if (model->beam_mic_mask)
        return model->beam_mic_mask;
    /* Anti-beam and the per-channel FIR want the measured path on every
     * microphone; the stock four-mic speech subset nulled noticeably
     * worse.  Without loaded IR taps there is no measured path at all,
     * so fall back to the stock speech set (lanes 0, 1, 3, 4). */
    return (mic_ir_ntaps > 0 ||
            (beam_view && beam_view->antibeam_available))
               ? LE_BEAM_ALL_MIC_MASK : 0x1BU;
}

/*
 * Optional adaptive residual stage after FIR + beamformer + highpass: a
 * single Speex canceller on the mono stream against the speaker reference lane,
 * mopping up what the static IR leaves behind (nonlinearities, drift).
 * Enabled by default; LE_MIC_POST_AEC=0 falls back to FIR-only output.
 */
static int mic_post_aec_enabled(void)
{
    const char *value = getenv("LE_MIC_POST_AEC");

    return !value || strcmp(value, "0") != 0;
}

/* Echo tail the post stage models, in milliseconds (128 default). */
static unsigned int mic_post_aec_tail_samples(void)
{
    const char *value = getenv("LE_MIC_POST_AEC_TAIL_MS");
    char *end = NULL;
    unsigned long parsed;
    unsigned int ms = MIC_POST_AEC_DEFAULT_TAIL_MS;

    if (value && *value) {
        errno = 0;
        parsed = strtoul(value, &end, 10);
        if (!errno && end != value && *end == '\0' &&
            parsed > 0 && parsed <= MIC_POST_AEC_MAX_TAIL_MS)
            ms = (unsigned int)parsed;
    }
    return ms * (MIC_RATE / 1000U);
}

/* Residual-echo suppressor depth in dB (negative). */
static int mic_post_aec_suppress_db(const char *name, int fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    long parsed;

    if (!value || !*value)
        return fallback;
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno || end == value || *end != '\0' ||
        parsed < -120 || parsed > 0)
        return fallback;
    return (int)parsed;
}

/* Writes the selected lanes as "0,1,3,4"; needs 14 bytes for all seven. */
static void format_mic_list(unsigned int mask, char *out, size_t size)
{
    size_t used = 0;
    unsigned int i;

    for (i = 0; i < LE_VOICE_LOGICAL_CHANNELS && used + 2 < size; ++i) {
        if (!(mask & (1U << i)))
            continue;
        if (used)
            out[used++] = ',';
        out[used++] = (char)('0' + i);
    }
    out[used] = '\0';
}

static void apply_digital_gain(int16_t *samples, size_t count,
                               unsigned int gain_hundredths)
{
    size_t i;

    if (gain_hundredths == MIC_DIGITAL_GAIN_UNITY)
        return;
    for (i = 0; i < count; ++i) {
        int32_t scaled = (int32_t)((samples[i] * (int32_t)gain_hundredths) /
                                   (int32_t)MIC_DIGITAL_GAIN_UNITY);

        if (scaled > INT16_MAX)
            scaled = INT16_MAX;
        else if (scaled < INT16_MIN)
            scaled = INT16_MIN;
        samples[i] = (int16_t)scaled;
    }
}

/*
 * Map the owner's 0..100 microphone gain onto the codec's MICPGA control.
 *
 * The four MICPGA volumes were fixed at 40, and configure_capture_path()
 * runs on every capture start -- not just at boot -- so anything that set
 * the gain elsewhere was overwritten as soon as the wake word listener
 * reopened the stream.  Restoring the level in audiod could never hold for
 * that reason, whatever order the daemons started in.
 *
 * 50 maps to 40 so an unconfigured device keeps exactly the level it had,
 * and the scale runs to 80 at 100, which is the control's maximum:
 *
 *   ADC_A MICPGA Volume Ctrl   080, 80  (range 0->80)
 *
 * Do not raise this ceiling.  An earlier attempt at 110 was out of range,
 * and rather than being clamped it left the capture about seven times
 * quieter than 80 -- so an out-of-range write here is not a no-op, it is
 * actively harmful.  Level beyond this comes from the digital stage above.  The upper half only
 * became usable once the 24-bit capture scaling was fixed; before that the
 * stream saturated regardless of any gain.
 *
 * The top of the range matters.  Scoring real captured audio through the
 * openwakeword alexa model, the device's own stream peaked at 0.41 against
 * a 0.45 gate with a single supporting frame -- a near miss on every
 * attempt.  The same audio amplified three times scores 0.70 with two
 * supporting frames and would fire, and six times scores 0.80.  The word
 * was being recognised all along and simply arriving too quiet.
 */
#define MICPGA_DEFAULT_VOLUME 40
#define MICPGA_MAX_VOLUME 80

#define MIC_CONFIG_MAX_BYTES 16384

/*
 * The owner's saved gain, read from the persisted web configuration named by
 * LE_MIC_CONFIG, or -1.  This is read on every capture start rather than once
 * at boot: LE_MIC_CAPTURE_GAIN is fixed when micd starts, so a gain changed
 * afterwards was reverted to the boot-time value the next time the wake
 * listener reopened the stream.
 */
static long saved_capture_gain(void)
{
    const char *path = getenv("LE_MIC_CONFIG");
    char text[MIC_CONFIG_MAX_BYTES];
    const char *p;
    char *end;
    long percent;
    size_t used;
    FILE *file;

    if (!path || !path[0] || !(file = fopen(path, "r")))
        return -1;
    used = fread(text, 1, sizeof(text) - 1, file);
    (void)fclose(file);
    text[used] = '\0';
    if (!(p = strstr(text, "\"microphone_gain\"")))
        return -1;
    p += strlen("\"microphone_gain\"");
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (*p++ != ':')
        return -1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (*p < '0' || *p > '9')
        return -1;
    errno = 0;
    percent = strtol(p, &end, 10);
    if (errno || percent > 100 || (*end != ',' && *end != '}' &&
        *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n'))
        return -1;
    return percent;
}

static int micpga_volume_from_environment(void)
{
    const char *text = getenv("LE_MIC_CAPTURE_GAIN");
    char *end;
    long percent = saved_capture_gain();

    if (percent >= 0)
        return (int)(percent * MICPGA_MAX_VOLUME / 100);
    if (!text || !text[0])
        return MICPGA_DEFAULT_VOLUME;
    errno = 0;
    percent = strtol(text, &end, 10);
    if (errno || *end || percent < 0 || percent > 100)
        return MICPGA_DEFAULT_VOLUME;
    return (int)(percent * MICPGA_MAX_VOLUME / 100);
}

static int configure_capture_path(const struct micd_config *config)
{
    char micpga[8];
    char default_micpga[8];
    struct mixer_setting micpga_settings[4];
    static const char *const micpga_controls[4] = {
        "ADC_A MICPGA Volume Ctrl", "ADC_B MICPGA Volume Ctrl",
        "ADC_C MICPGA Volume Ctrl", "ADC_D MICPGA Volume Ctrl"
    };
    static const struct mixer_setting settings[] = {
        {"Mic PGA Switch", "1", "1"},
        {"ADCFGA Left Mute Switch", "0", NULL},
        {"ADCFGA Right Mute Switch", "0", NULL},
        {"ADC_A Left Ip Select ADC_A DIF1_L switch", "1", NULL},
        {"ADC_A Right Ip Select ADC_A DIF1_R switch", "1", NULL},
        {"ADC_B Left Ip Select ADC_B DIF1_L switch", "1", NULL},
        {"ADC_B Right Ip Select ADC_B DIF1_R switch", "1", NULL},
        {"ADC_C Left Ip Select ADC_C DIF1_L switch", "1", NULL},
        {"ADC_C Right Ip Select ADC_C DIF1_R switch", "1", NULL},
        {"ADC_D Left Ip Select ADC_D DIF1_L switch", "1", NULL},
        {"ADC_D Right Ip Select ADC_D DIF1_R switch", "1", NULL},
        {"ADC_A DIF1_L Input Gain", "1", NULL},
        {"ADC_A DIF1_R Input Gain", "1", NULL},
        {"ADC_B DIF1_L Input Gain", "1", NULL},
        {"ADC_B DIF1_R Input Gain", "1", NULL},
        {"ADC_C DIF1_L Input Gain", "1", NULL},
        {"ADC_C DIF1_R Input Gain", "1", NULL},
        {"ADC_D DIF1_L Input Gain", "1", NULL},
        {"ADC_D DIF1_R Input Gain", "1", NULL},
        {"SpiTimeStamps", "0", NULL}
    };
    size_t i;

    for (i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        if (apply_mixer_setting(config->mixer_bin, &settings[i]) < 0) {
            le_log_error("unable to apply microphone mixer control: %s",
                         settings[i].control);
            return -1;
        }
    }
    (void)snprintf(micpga, sizeof(micpga), "%d",
                   micpga_volume_from_environment());
    (void)snprintf(default_micpga, sizeof(default_micpga), "%d",
                   MICPGA_DEFAULT_VOLUME);
    for (i = 0; i < 4; ++i) {
        micpga_settings[i].control = micpga_controls[i];
        micpga_settings[i].value = micpga;
        micpga_settings[i].value2 = micpga;
        if (apply_mixer_setting(config->mixer_bin, &micpga_settings[i]) < 0) {
            /*
             * The control's usable maximum is a property of the codec, not
             * of this scale.  If a value is refused, fall back to the level
             * the capture path used before rather than failing the whole
             * profile -- an unusable microphone is far worse than a quiet
             * one, and configure_capture_path() failing leaves the device
             * with no capture at all.
             */
            struct mixer_setting fallback;

            le_log_warn("micd: capture PGA %s refused for %s; using %d",
                        micpga, micpga_controls[i], MICPGA_DEFAULT_VOLUME);
            fallback.control = micpga_controls[i];
            fallback.value = default_micpga;
            fallback.value2 = default_micpga;
            if (apply_mixer_setting(config->mixer_bin, &fallback) < 0) {
                le_log_error("unable to apply microphone mixer control: %s",
                             micpga_controls[i]);
                return -1;
            }
        }
    }
    le_log_info("micd: capture PGA set to %s", micpga);
    return 0;
}

static int format_status(char *buffer, size_t size,
                         const struct micd_config *config,
                         int capture_active, int capture_configured)
{
    int values[MIC_LOGICAL_CHANNELS];
    int count = read_miccal(config->idme_dir, config->dt_idme_dir, values);
    int available = access(config->pcm_path, R_OK) == 0;
    struct mic_model model;
    char mics[16];
    int n;

    mic_model_load(config->dt_idme_dir, &model);
    format_mic_list(beam_mic_mask(&model), mics, sizeof(mics));
    n = snprintf(
        buffer, size,
        "{\"available\":%s,\"simulated\":false,"
        "\"sources\":[{\"id\":\"0:24\",\"card\":0,\"device\":24,"
        "\"name\":\"Amazon MT8163 microphone array\","
        "\"rate\":16000,\"channels\":9,\"bits\":24,\"valid_bits\":16,"
        "\"encoding\":\"pcm_s24_3le\","
        "\"microphones\":["
        "{\"channel\":0,\"name\":\"Array microphone 1\"},"
        "{\"channel\":1,\"name\":\"Array microphone 2\"},"
        "{\"channel\":2,\"name\":\"Array microphone 3\"},"
        "{\"channel\":3,\"name\":\"Array microphone 4\"},"
        "{\"channel\":4,\"name\":\"Array microphone 5\"},"
        "{\"channel\":5,\"name\":\"Array microphone 6\"},"
        "{\"channel\":6,\"name\":\"Array microphone 7\"}]}],"
        "\"raw_channels\":9,\"active_microphone_channels\":7,"
        "\"inactive_transport_channels\":[7,8],\"logical_channels\":7,"
        "\"capture\":{\"active\":%s,\"owner\":%s},"
        "\"hardware_profile\":{\"name\":\"radar-audio-device-v4.02\","
        "\"configured\":%s,\"adc_codecs\":4,\"analog_pga_db\":20},"
        "\"calibration\":{\"q14\":[%d,%d,%d,%d,%d,%d,%d],"
        "\"fallback\":16384,\"source\":\"%s\","
        "\"values_found\":%d,\"complete\":%s,"
        "\"selected_logical_mics\":[%s],"
        "\"polarity\":\"all-positive-measured\","
        "\"applied_to_raw_stream\":false,"
        "\"applied_to_calibrated_stream\":true,"
        "\"mapping_available\":true,"
        "\"mapping\":\"measured; lane n is physical microphone n\","
        "\"geometry\":{\"reference\":\"0 deg front (volume down), clockwise viewed from above\",\"ring_lanes\":[0,1,2,3,4,5],\"ring_azimuth_deg\":[150,210,270,330,30,90],\"centre_lane\":6,\"ring_radius_mm\":36,\"lane_skew\":\"none measured\"},"
        "\"relative_delay_samples\":{}},"
        "\"pipeline\":{\"stage\":\"fixed-beam-select\","
        "\"microphone_mix\":\"%s\",\"microphone_lane\":%d,"
        "\"antibeam_available\":%s,"
        "\"beams\":%d,\"fft\":%d,\"hop\":%d,"
        "\"beam_design\":\"superdirective, white-noise gain >= -3 dB\","
        "\"highpass_hz\":120,\"vad\":\"native-energy-baseline\","
        "\"aec\":\"anti-beam-ir\",\"beamforming\":true,\"wake_word\":false}}",
        available ? "true" : "false",
        capture_active ? "true" : "false",
        capture_active ? "\"raw-stream\"" : "null",
        capture_configured ? "true" : "false",
        values[0], values[1], values[2], values[3],
        values[4], values[5], values[6],
        miccal_source(config->idme_dir, config->dt_idme_dir),
        count, count == MIC_LOGICAL_CHANNELS ? "true" : "false",
        mics, le_mic_mix_name(current_mix_mode()), current_mix_lane(),
        beam_view && beam_view->antibeam_available ? "true" : "false",
        LE_BEAM_DEFAULT_BEAMS, LE_BEAM_FFT, LE_BEAM_HOP);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

static int parse_channel(const char *args, int *channel)
{
    const char *key;
    const char *colon;
    char *end;
    long value;

    if (!args || !channel)
        return -1;
    key = strstr(args, "\"channel\"");
    if (!key)
        return -1;
    colon = strchr(key + 9, ':');
    if (!colon)
        return -1;
    errno = 0;
    value = strtol(colon + 1, &end, 10);
    if (errno || end == colon + 1 || value < 0 ||
        value >= MIC_CAPTURE_CHANNELS)
        return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        ++end;
    if (*end != ',' && *end != '}')
        return -1;
    *channel = (int)value;
    return 0;
}

static int relay_capture(int client_fd, const struct micd_config *config)
{
    int pipe_fds[2];
    pid_t capture;
    char buffer[8192];
    ssize_t count;

    if (pipe(pipe_fds) < 0)
        return -1;
    capture = fork();
    if (capture < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (capture == 0) {
        char *const argv[] = {
            (char *)"tinycap", (char *)"--",
            (char *)"-D", (char *)"0",
            (char *)"-d", (char *)"24",
            (char *)"-c", (char *)"9",
            (char *)"-r", (char *)"16000",
            (char *)"-b", (char *)"24",
            (char *)"-p", (char *)"640",
            /*
             * 640 frames is 40 ms at 16 kHz.  Four periods consume the
             * Amazon SPI PCM driver's full 69120-byte ring and provide
             * enough scheduling headroom for the wake inference thread.
             */
            (char *)"-n", (char *)"4",
            NULL
        };
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(pipe_fds[1]);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
            _exit(127);
        if (getppid() == 1)
            _exit(127);
        execv(config->capture_bin, argv);
        _exit(127);
    }
    close(pipe_fds[1]);
    while ((count = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        if (write_all(client_fd, buffer, (size_t)count) < 0)
            break;
    }
    close(pipe_fds[0]);
    kill(capture, SIGTERM);
    while (waitpid(capture, NULL, 0) < 0 && errno == EINTR)
        ;
    return count < 0 ? -1 : 0;
}

static pid_t start_capture(const struct micd_config *config, int output_fd)
{
    pid_t capture = fork();

    if (capture != 0)
        return capture;
    {
        char *const argv[] = {
            (char *)"tinycap", (char *)"--",
            (char *)"-D", (char *)"0",
            (char *)"-d", (char *)"24",
            (char *)"-c", (char *)"9",
            (char *)"-r", (char *)"16000",
            (char *)"-b", (char *)"24",
            (char *)"-p", (char *)"640",
            (char *)"-n", (char *)"4",
            NULL
        };

        if (dup2(output_fd, STDOUT_FILENO) < 0)
            _exit(127);
        if (output_fd != STDOUT_FILENO)
            close(output_fd);
        /* Kill tinycap automatically when the relay worker dies, so a
         * crashed worker cannot leave an orphaned capture holding the
         * PCM device busy for every later stream. */
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
            _exit(127);
        if (getppid() == 1)
            _exit(127);
        execv(config->capture_bin, argv);
        _exit(127);
    }
}

/*
 * Measured echo-path model: per-microphone impulse responses of the
 * speaker reference lane (capture.ref_lane in the model profile),
 * measured once by the sweep calibration.
 * The anti-beam projection uses them as the per-frequency speaker-to-mic
 * steering path. File format: LE_VOICE_LOGICAL_CHANNELS x N int16 Q15
 * taps, mic-major.
 */
#define MIC_IR_MAX_TAPS 512
/* Taps are stored reversed at load time: that is the layout the anti-beam
 * projection loader consumes directly (reversed=1). */
static int16_t mic_ir_taps[LE_VOICE_LOGICAL_CHANNELS][MIC_IR_MAX_TAPS];
/* Mirrored reference history for the per-channel FIR. */
static int16_t mic_ir_hist[2 * MIC_IR_MAX_TAPS];
static unsigned int mic_ir_pos;
static unsigned int mic_ir_mask;

static int mic_ir_load(const char *path)
{
    FILE *f;
    int16_t buf[LE_VOICE_LOGICAL_CHANNELS * MIC_IR_MAX_TAPS];
    size_t n;

    f = fopen(path, "rb");
    if (!f)
        return -1;
    n = fread(buf, sizeof(*buf), sizeof(buf) / sizeof(*buf), f);
    fclose(f);
    if (n % LE_VOICE_LOGICAL_CHANNELS || n < LE_VOICE_LOGICAL_CHANNELS)
        return -1;
    mic_ir_ntaps = (int)(n / LE_VOICE_LOGICAL_CHANNELS);
    if (mic_ir_ntaps > MIC_IR_MAX_TAPS)
        mic_ir_ntaps = MIC_IR_MAX_TAPS;
    {
        /* store taps reversed: taps_rev[k] = taps[N-1-k] pairs the
         * oldest->newest window into a normal convolution */
        int c, k;
        for (c = 0; c < LE_VOICE_LOGICAL_CHANNELS; ++c)
            for (k = 0; k < mic_ir_ntaps; ++k)
                mic_ir_taps[c][k] =
                    buf[c * mic_ir_ntaps + mic_ir_ntaps - 1 - k];
    }
    memset(mic_ir_hist, 0, sizeof(mic_ir_hist));
    mic_ir_pos = 0;
    le_log_info("mic ir: %d taps/mic", mic_ir_ntaps);
    return 0;
}

/* Amazon device type id ("A3S5BH2HU6VAYF" for the second-gen Echo) from
 * the idme area; used to pick the shipped per-model IR taps. */
static int read_device_type_id(const char *dt_idme_dir, char *out,
                               size_t out_size)
{
    char path[256];
    FILE *f;
    size_t n;

    if (snprintf(path, sizeof(path), "%s/device_type_id/value",
                 dt_idme_dir) >= (int)sizeof(path))
        return -1;
    f = fopen(path, "rb");
    if (!f)
        return -1;
    n = fread(out, 1, out_size - 1, f);
    fclose(f);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\0' ||
                 out[n - 1] == ' ' || out[n - 1] == '\t'))
        --n;
    out[n] = '\0';
    return n ? 0 : -1;
}

/* IR taps resolution: LE_MIC_IR_FILE wins (experiments), then the
 * per-device calibration /data/libreecho/echo-ir.bin, then the shipped
 * per-model file /usr/local/share/libreecho/ir/<device_type_id>.bin.
 * LE_MIC_IR_DIR relocates the shipped directory. */
static const char *mic_ir_resolve(const char *dt_idme_dir,
                                  const struct mic_model *model)
{
    static char path[320];
    const char *env = getenv("LE_MIC_IR_FILE");
    const char *dir = getenv("LE_MIC_IR_DIR");
    char id[64];

    if (env && env[0])
        return env;
    if (access("/data/libreecho/echo-ir.bin", R_OK) == 0)
        return "/data/libreecho/echo-ir.bin";
    if (!dir)
        dir = "/usr/local/share/libreecho/ir";
    if (model->ir_file[0] &&
        snprintf(path, sizeof(path), "%s/%s", dir, model->ir_file) <
            (int)sizeof(path) &&
        access(path, R_OK) == 0)
        return path;
    if (read_device_type_id(dt_idme_dir, id, sizeof(id)) == 0 &&
        snprintf(path, sizeof(path), "%s/%s.bin", dir, id) <
            (int)sizeof(path) &&
        access(path, R_OK) == 0)
        return path;
    return NULL;
}

/* LE_MIC_FIR_NEON=0 forces the scalar dot product for A/B comparison. */
static int mic_fir_use_neon(void)
{
    static int cached = -1;
    const char *value;

    if (cached < 0) {
        value = getenv("LE_MIC_FIR_NEON");
        cached = !value || strcmp(value, "0") != 0;
    }
    return cached;
}

static int64_t mic_fir_dot(const int16_t *a, const int16_t *b, int n)
{
    int64_t acc = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (mic_fir_use_neon()) {
        int32x4_t v = vdupq_n_s32(0);
        for (; n >= 8; n -= 8, a += 8, b += 8) {
            int16x8_t qa = vld1q_s16(a), qb = vld1q_s16(b);
            v = vmlal_s16(v, vget_low_s16(qa), vget_low_s16(qb));
            v = vmlal_s16(v, vget_high_s16(qa), vget_high_s16(qb));
        }
        {
            int32x2_t s = vadd_s32(vget_low_s32(v), vget_high_s32(v));
            acc = (int64_t)vget_lane_s32(s, 0) + vget_lane_s32(s, 1);
        }
    }
#else
    (void)mic_fir_use_neon;
#endif
    for (; n > 0; --n)
        acc += (int64_t)(*a++) * (*b++);
    return acc;
}

/* conv(ref, ir[channel]) subtracted from one microphone sample. */
static int16_t mic_ir_subtract(unsigned int channel, int16_t sample)
{
    int64_t acc = mic_fir_dot(mic_ir_taps[channel],
                              mic_ir_hist + mic_ir_pos + 1, mic_ir_ntaps);
    acc = sample - (acc >> 15);
    if (acc > INT16_MAX)
        acc = INT16_MAX;
    if (acc < INT16_MIN)
        acc = INT16_MIN;
    return (int16_t)acc;
}

/* Polls a small status file for a literal flag substring; re-reads only
 * when mtime changed.  Used to follow esphomed's voice-turn state. */
static int mic_file_flag_poll(const char *path, long *sec, long *nsec,
                              const char *flag, int current)
{
    struct stat st;
    FILE *f;
    char buf[512];
    size_t n;

    if (stat(path, &st) != 0)
        return current;
    if ((long)st.st_mtime == *sec && (long)st.st_mtim.tv_nsec == *nsec)
        return current;
    *sec = (long)st.st_mtime;
    *nsec = (long)st.st_mtim.tv_nsec;
    f = fopen(path, "r");
    if (!f)
        return current;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, flag) != NULL;
}

/*
 * Streams calibrated mono audio. The OK header is the only success signal a
 * client gets, so it is written once tinycap has delivered its first period.
 * If tinycap exits first (the PCM is busy or missing), the client gets an
 * error and not an OK header followed by an empty stream.
 */
static int relay_capture_mono(int client_fd, unsigned long id,
                              const char *header, size_t header_size,
                              const struct micd_config *config)
{
    static const uint8_t identity_map[LE_VOICE_LOGICAL_CHANNELS] =
        {0, 1, 2, 3, 4, 5, 6};
    static struct le_beamformer beamformer;
    struct le_voice_calibration calibration;
    struct le_voice_highpass highpass;
    struct mic_model model;
    struct le_voice_highpass ref_highpass;
    unsigned char input[LE_VOICE_RAW_FRAME_BYTES * 256];
    int16_t output[512];
    int16_t post_in[MIC_POST_AEC_FRAME_SAMPLES];
    int16_t post_ref[MIC_POST_AEC_FRAME_SAMPLES];
    int16_t post_out[MIC_POST_AEC_FRAME_SAMPLES];
    unsigned int post_fill = 0;
    int post_aec_on = 0;
    int speex_on = 1;
    struct le_voice_aec post_aec;
    int miccal[LE_VOICE_LOGICAL_CHANNELS];
    size_t pending = 0;
    int pipe_fds[2];
    pid_t capture;
    ssize_t count = 0;
    unsigned int digital_gain = digital_gain_from_environment();
    unsigned int mix_mask = LE_BEAM_DEFAULT_MIC_MASK;
    int result = -1;

    if (pipe(pipe_fds) < 0)
        return -1;
    capture = start_capture(config, pipe_fds[1]);
    if (capture < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    close(pipe_fds[1]);

    /*
     * This pinned tinycap treats "--" as a raw-stdout transport, so the
     * first byte read here is microphone PCM rather than a WAV header.
     */
    (void)read_miccal(config->idme_dir, config->dt_idme_dir, miccal);
    le_voice_calibration_init(
        &calibration, miccal, identity_map,
        LE_VOICE_CALIBRATION_DIRECT_Q14);
    /*
     * Static measured IR is the only echo cancellation: LE_MIC_IR_FILE
     * overrides the default location.
     */
    mic_model_load(config->dt_idme_dir, &model);
    if (model.mic_pos_count)
        le_beamformer_set_positions(model.mic_x_um, model.mic_y_um,
                                    model.mic_pos_count);
    mic_ir_ntaps = 0;
    {
        const char *ir_path = mic_ir_resolve(config->dt_idme_dir, &model);
        if (ir_path && mic_ir_load(ir_path) == 0)
            le_log_info("mic ir: taps from %s", ir_path);
    }
    if (le_beamformer_init(&beamformer, beam_mic_mask(&model),
                           LE_BEAM_DEFAULT_BEAMS) < 0) {
        close(pipe_fds[0]);
        kill(capture, SIGTERM);
        while (waitpid(capture, NULL, 0) < 0 && errno == EINTR)
            ;
        return -1;
    }
    /*
     * LE_MIC_ANTIBEAM=1: spatial null onto the measured speaker path H(k)
     * before beam synthesis; output becomes the projected channel sum.
     * LE_MIC_ANTIBEAM=auto: same, but only while the playback
     * reference carries energy (media playback), with hold-off hysteresis.
     * Uses the same IR file as the static FIR subtraction.
     */
    int ab_loaded = 0;
    /* ab_force: LE_MIC_ANTIBEAM set to anything but "auto"/"0" keeps the
     * null on permanently.  ab_env_auto: "auto" gates on playback.
     * Without the env var the "antibeam" mix mode (GUI/API, persisted in
     * the mic config) gates the same way — taps load whenever the
     * measured IR file exists so the mode can be switched live. */
    int ab_force = 0, ab_env_auto = 0;
    {
        const char *ab = getenv("LE_MIC_ANTIBEAM");
        if (mic_ir_ntaps > 0 &&
            le_beamformer_antibeam_load(&beamformer,
                                        (const int16_t *)mic_ir_taps,
                                        mic_ir_ntaps,
                                        LE_VOICE_LOGICAL_CHANNELS, 1) == 0) {
            ab_loaded = 1;
            if (beam_view)
                beam_view->antibeam_available = 1;
            ab_env_auto = ab && !strcmp(ab, "auto");
            ab_force = ab && !ab_env_auto && strcmp(ab, "0");
            beamformer.antibeam = ab_force;
            le_log_info("antibeam: %s, %d taps x %u mics",
                        ab_force ? "always"
                                 : (ab_env_auto ? "auto (follows playback)"
                                                : "mode-selectable"),
                        mic_ir_ntaps, beamformer.mic_count);
        } else if (ab && strcmp(ab, "0")) {
            le_log_error("antibeam: load failed, disabled");
        }
    }
    /* Residual IR post-filter behind the anti-beam (see above): only
     * armed when taps load AND the projection can render. */
    /* Per-channel static FIR against the speaker reference lane: the stage
     * that removed the bulk of the measured speaker path in the old
     * build, ahead of beamforming/anti-beam.  On by default whenever
     * the measured IR loaded; LE_MIC_IR_SUBTRACT=0 or the runtime
     * "set_fir" command disables it. */
    int ir_subtract = 0;
    {
        const char *irs = getenv("LE_MIC_IR_SUBTRACT");
        ir_subtract = mic_ir_ntaps > 0 && (!irs || strcmp(irs, "0") != 0);
        mic_ir_mask = (1U << LE_VOICE_LOGICAL_CHANNELS) - 1;
        if (ir_subtract)
            le_log_info("mic fir: %d taps x %u channels, mask 0x%x",
                        mic_ir_ntaps, LE_VOICE_LOGICAL_CHANNELS,
                        mic_ir_mask);
    }
    float ab_ref_env = 0.0f;
    long ab_hold = 0;
    /* While a voice turn is in progress (wake-word capture + STT) the
     * engine ducks media fully; the projection is then unnecessary and
     * would only colour the speech, so it is forced off. */
    const char *turn_status = getenv("LE_MIC_TURN_STATUS");
    long turn_sec = -1, turn_nsec = -1;
    unsigned int ab_gate_ticks = 0;
    int ab_turn_gate = 0;
    long gate_hold = 0;
    int wake_fd = -1;
    int wake_sub_sent = 0;
    const char *wake_path = getenv("LE_MIC_WAKE_SOCK");
    if (!turn_status || !turn_status[0])
        turn_status = "/run/libreecho/esphome-status.json";
    if (!wake_path || !wake_path[0])
        wake_path = "/run/libreecho/wakeword.sock";
    if (mic_post_aec_enabled()) {
        int suppress = mic_post_aec_suppress_db(
            "LE_MIC_POST_AEC_SUPPRESS_DB", -40);
        int active = mic_post_aec_suppress_db(
            "LE_MIC_POST_AEC_SUPPRESS_ACTIVE_DB", -15);
        unsigned int tail = mic_post_aec_tail_samples();

        if (le_voice_aec_init_suppress(&post_aec,
                                       MIC_POST_AEC_FRAME_SAMPLES,
                                       tail, suppress, active) == 0) {
            const char *dn_env = getenv("LE_MIC_POST_AEC_DENOISE");

            if (dn_env && atoi(dn_env) != 0 &&
                le_voice_aec_set_denoise(&post_aec, 1) == 0) {
                const char *ndb_env =
                    getenv("LE_MIC_POST_AEC_NOISE_DB");

                if (ndb_env)
                    le_voice_aec_set_noise_db(&post_aec,
                                              atoi(ndb_env));
                le_log_info("mic post-aec: denoise on%s%s",
                            ndb_env ? ", noise " : "",
                            ndb_env ? ndb_env : "");
            }
            post_aec_on = 1;
            const char *sx_env = getenv("LE_MIC_POST_AEC_SPEEX");

            if (sx_env && !strcmp(sx_env, "0"))
                speex_on = 0;
            le_log_info(
                "mic post-aec: %s tail %u suppress %d/%d dB",
                speex_on ? "speex" : "passthrough",
                tail, suppress, active);
        } else {
            le_log_error("mic post-aec: init failed, disabled");
        }
    }
    le_voice_highpass_init(&highpass);
    le_voice_highpass_init(&ref_highpass);
    le_voice_highpass_set_hz(&highpass, 120.f);
    le_voice_highpass_set_hz(&ref_highpass, 120.f);
    mix_mask = beam_mic_mask(&model);

    count = read(pipe_fds[0], input, sizeof(input));
    if (count <= 0) {
        /* tinycap exited before its first period: no capture, say so. */
        char error[LE_ADAPTER_MSG_MAX];
        int n = le_adapter_respond_err(error, sizeof(error), id,
                                       "microphone capture unavailable");

        if (n > 0)
            (void)write_all(client_fd, error, (size_t)n);
    } else if (write_all(client_fd, header, header_size) == 0) {
        while ((count = read(pipe_fds[0], input + pending,
                             sizeof(input) - pending)) > 0) {
            size_t available = pending + (size_t)count;
            size_t frames = available / LE_VOICE_RAW_FRAME_BYTES;
            size_t consumed = frames * LE_VOICE_RAW_FRAME_BYTES;

        if (frames > 0) {
            size_t frame;
            size_t out_pos = 0;

            for (frame = 0; frame < frames; ++frame) {
                const uint8_t *raw = input + frame * LE_VOICE_RAW_FRAME_BYTES;
                int16_t logical[LE_VOICE_LOGICAL_CHANNELS];
                int16_t ref = 0;
                int16_t sample;
                unsigned int channel;

                le_voice_decode_calibrated(raw, &calibration, logical);
                /* Mix mode: anti-beam is selectable like the other modes
                 * (set_mix / saved config); LE_MIC_ANTIBEAM keeps working
                 * as a start-up override. */
                unsigned int mode = current_mix_mode();
                int ab_gated = ab_loaded &&
                               (ab_env_auto || mode == LE_MIC_MIX_ANTIBEAM);
                if (ir_subtract || post_aec_on || ab_gated)
                    ref = le_voice_unpack_s24_3le(
                        raw + model.ref_lane *
                                LE_VOICE_BYTES_PER_SAMPLE);
                if (ir_subtract &&
                    (!beam_view || beam_view->fir_enable)) {
                    int16_t fed[LE_VOICE_LOGICAL_CHANNELS];

                    mic_ir_hist[mic_ir_pos] = ref;
                    mic_ir_hist[mic_ir_pos + mic_ir_ntaps] = ref;
                    for (channel = 0;
                         channel < LE_VOICE_LOGICAL_CHANNELS; ++channel)
                        fed[channel] = (mic_ir_mask & (1U << channel))
                            ? mic_ir_subtract(channel, logical[channel])
                            : logical[channel];
                    if (++mic_ir_pos == (unsigned int)mic_ir_ntaps)
                        mic_ir_pos = 0;
                    memcpy(logical, fed, sizeof(fed));
                }
                if (ab_gated || post_aec_on) {
                    /* Voice-turn gate: opens instantly on waked's
                     * wake_detected broadcast (no esphomed latency),
                     * stays open while in_progress is true plus ~1 s,
                     * or 4 s after a bare wake event (covers turns that
                     * never reach esphomed). */
                    /* Socket setup/IO only on the ~20 ms tick: a
                     * per-sample recv/connect-retry storm would itself
                     * starve the real-time loop. */
                    if (++ab_gate_ticks >= 320) {
                        ab_gate_ticks = 0;
                        if (wake_fd < 0) {
                            struct sockaddr_un wa;

                            wake_fd = socket(AF_UNIX,
                                             SOCK_STREAM | SOCK_NONBLOCK |
                                             SOCK_CLOEXEC, 0);
                            if (wake_fd >= 0) {
                                memset(&wa, 0, sizeof(wa));
                                wa.sun_family = AF_UNIX;
                                snprintf(wa.sun_path,
                                         sizeof(wa.sun_path), "%s",
                                         wake_path);
                                if (connect(wake_fd,
                                            (struct sockaddr *)&wa,
                                            sizeof(wa)) < 0 &&
                                    errno != EINPROGRESS) {
                                    close(wake_fd);
                                    wake_fd = -1;
                                }
                            }
                        }
                        if (wake_fd >= 0 && !wake_sub_sent) {
                            static const char sub[] =
                                "{\"v\":1,\"id\":1,\"cmd\":\"subscribe\","
                                "\"args\":{}}\n";

                            if (send(wake_fd, sub, sizeof(sub) - 1,
                                     MSG_NOSIGNAL) >= 0)
                                wake_sub_sent = 1;
                        }
                        if (wake_fd >= 0) {
                            char wbuf[1024];
                            ssize_t wn = recv(wake_fd, wbuf,
                                              sizeof(wbuf) - 1, 0);

                            if (wn == 0 ||
                                (wn < 0 && errno != EAGAIN &&
                                 errno != EWOULDBLOCK)) {
                                close(wake_fd);
                                wake_fd = -1;
                                wake_sub_sent = 0;
                            } else if (wn > 0) {
                                wbuf[wn] = '\0';
                                if (strstr(wbuf, "wake_detected")) {
                                    ab_turn_gate = 1;
                                    if (gate_hold < 64000)
                                        gate_hold = 64000;
                                }
                            }
                        }
                        if (mic_file_flag_poll(
                                turn_status, &turn_sec, &turn_nsec,
                                "\"in_progress\":true", 0)) {
                            ab_turn_gate = 1;
                            if (gate_hold < 64000)
                                gate_hold = 64000;
                        } else if (gate_hold > 0) {
                            gate_hold -= 320;
                            if (gate_hold <= 0)
                                ab_turn_gate = 0;
                        }
                    }
                }
                if (ab_gated) {
                    /* Playback envelope on the reference lane: peak-hold with
                     * fast decay; anti-beam on above on-threshold, off after
                     * ~2 s below off-threshold (hysteresis + hold). */
                    float a = ref < 0 ? -(float)ref : (float)ref;
                    if (a > ab_ref_env)
                        ab_ref_env = a;
                    else
                        ab_ref_env *= 0.9995f;
                    if (ab_ref_env > 500.0f)
                        ab_hold = 32000;        /* 2 s @16 kHz */
                    else if (ab_hold > 0)
                        --ab_hold;
                    beamformer.antibeam = (ab_hold > 0) && !ab_turn_gate;
                } else {
                    beamformer.antibeam = ab_force;
                }
                {
                    int16_t mixed =
                        le_beamformer_sample(&beamformer, logical);

                    if (mode == LE_MIC_MIX_AVERAGE)
                        mixed = le_mic_mix_average(logical, mix_mask);
                    else if (mode == LE_MIC_MIX_SINGLE)
                        mixed = le_mic_mix_single(logical, mix_mask,
                                                  current_mix_lane());
                    sample =
                        le_voice_highpass_sample(&highpass, mixed);
                }
                /* In passthrough mode (no speex) the post path runs
                 * always — the AGC gain simply fades between adaptive
                 * (turn) and 1.0 (idle), which makes the switch
                 * click-free and loss-free by construction. */
                if (post_aec_on && (ab_turn_gate || !speex_on)) {
                    post_in[post_fill] = sample;
                    post_ref[post_fill] =
                        le_voice_highpass_sample(&ref_highpass, ref);
                    if (++post_fill == MIC_POST_AEC_FRAME_SAMPLES) {
                        post_fill = 0;
                        if (speex_on)
                            le_voice_aec_process(&post_aec, post_in,
                                                 post_ref, post_out);
                        else
                            memcpy(post_out, post_in,
                                   sizeof(post_in));
                        apply_digital_gain(post_out,
                                           MIC_POST_AEC_FRAME_SAMPLES,
                                           digital_gain);
                        if (write_all(client_fd, post_out,
                                      sizeof(post_out)) < 0)
                            goto relay_done;
                    }
                } else {
                    /* Turn just ended: emit the partial buffered frame raw
                     * instead of dropping it — avoids an up-to-10 ms gap
                     * at the path switch. */
                    if (post_aec_on && post_fill > 0) {
                        unsigned int fi;

                        for (fi = 0; fi < post_fill &&
                                    out_pos < sizeof(output) /
                                              sizeof(output[0]); ++fi)
                            output[out_pos++] = post_in[fi];
                        post_fill = 0;
                    }
                    output[out_pos++] = sample;
                }
                if (beamformer.fill == 0)
                    beam_view_publish(&beamformer);
            }
            if (out_pos > 0) {
                apply_digital_gain(output, out_pos, digital_gain);
                if (write_all(client_fd, output,
                              out_pos * sizeof(output[0])) < 0)
                    break;
            }
        }
        pending = available - consumed;
        if (pending > 0)
            memmove(input, input + consumed, pending);
        }
    }
relay_done:
    result = count < 0 ? -1 : 0;
    if (post_aec_on)
        le_voice_aec_destroy(&post_aec);

    beam_view_stop();
    close(pipe_fds[0]);
    kill(capture, SIGTERM);
    while (waitpid(capture, NULL, 0) < 0 && errno == EINTR)
        ;
    return result;
}

static void run_stream_worker(int client_fd, unsigned long id, int channel,
                              int calibrated_mono,
                              const struct micd_config *config)
{
    char data[512];
    char response[768];
    int n;

    if (calibrated_mono) {
        struct mic_model model;
        char mics[16];

        mic_model_load(config->dt_idme_dir, &model);
        format_mic_list(beam_mic_mask(&model), mics, sizeof(mics));
        n = snprintf(
            data, sizeof(data),
            "{\"format\":\"pcm_s16_le\",\"rate\":16000,"
            "\"channels\":1,\"bits\":16,\"valid_bits\":16,"
            "\"selected_logical_mics\":[%s],"
            "\"calibration_applied\":true,"
            "\"calibration_mode\":\"direct_q14\","
            "\"logical_map\":\"measured\","
            "\"beamforming\":\"fixed_beam_select\","
            "\"beams\":%d,\"latency_samples\":%d,"
            "\"highpass_hz\":120}", mics, LE_BEAM_DEFAULT_BEAMS, LE_BEAM_FFT);
    } else {
        n = snprintf(data, sizeof(data),
                     "{\"format\":\"pcm_s24_3le\",\"rate\":16000,"
                     "\"channels\":9,\"bits\":24,\"valid_bits\":16,"
                     "\"selected_raw_channel\":%d,"
                     "\"calibration_applied\":false}", channel);
    }
    if (n > 0 && (size_t)n < sizeof(data))
        n = le_adapter_respond_ok(response, sizeof(response), id, data);
    else
        n = -1;
    if (n > 0) {
        if (calibrated_mono) {
            /* relay_capture_mono sends this header once capture has started. */
            (void)relay_capture_mono(client_fd, id, response, (size_t)n,
                                     config);
        } else if (write_all(client_fd, response, (size_t)n) == 0) {
            (void)relay_capture(client_fd, config);
        }
    }
    close(client_fd);
}

static int respond(int client_fd, unsigned long id, int ok,
                   const char *payload)
{
    char response[LE_ADAPTER_MSG_MAX];
    int n = ok
        ? le_adapter_respond_ok(response, sizeof(response), id, payload)
        : le_adapter_respond_err(response, sizeof(response), id, payload);

    return n < 0 ? -1 : write_all(client_fd, response, (size_t)n);
}

int main(int argc, char **argv)
{
    struct micd_config config = {
        LE_ADAPTER_MIC_SOCK,
        "/proc/idme",
        "/sys/firmware/devicetree/base/idme",
        "/dev/snd/pcmC0D24c",
        "/sbin/tinycap",
        "/sbin/tinymix"
    };
    pid_t stream_worker = -1;
    int capture_configured = 0;
    int listen_fd;
    int i;

    le_log_init("micd", argc, argv);
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--foreground")) {
            continue;
        } else if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            config.socket_path = argv[++i];
        } else if (!strcmp(argv[i], "--idme-dir") && i + 1 < argc) {
            config.idme_dir = argv[++i];
        } else if (!strcmp(argv[i], "--dt-idme-dir") && i + 1 < argc) {
            config.dt_idme_dir = argv[++i];
        } else if (!strcmp(argv[i], "--pcm-path") && i + 1 < argc) {
            config.pcm_path = argv[++i];
        } else if (!strcmp(argv[i], "--capture-bin") && i + 1 < argc) {
            config.capture_bin = argv[++i];
        } else if (!strcmp(argv[i], "--mixer-bin") && i + 1 < argc) {
            config.mixer_bin = argv[++i];
        } else if (!strcmp(argv[i], "--verbose") ||
                   !strcmp(argv[i], "--debug") ||
                   !strcmp(argv[i], "--quiet") ||
                   !strcmp(argv[i], "--syslog")) {
            continue;
        } else {
            fprintf(stderr, "Usage: %s [--foreground] [--socket PATH] "
                    "[--idme-dir DIR] [--pcm-path PATH] "
                    "[--dt-idme-dir DIR] "
                    "[--capture-bin PATH] [--mixer-bin PATH]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGCHLD, child_handler);
    signal(SIGPIPE, SIG_IGN);
    if (access(config.pcm_path, R_OK) == 0) {
        capture_configured = configure_capture_path(&config) == 0;
        if (capture_configured)
            le_log_info("Radar microphone mixer profile configured");
        else
            le_log_warn("microphone endpoint exists but mixer setup failed");
    }
    beam_view = mmap(NULL, sizeof(*beam_view), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (beam_view == MAP_FAILED)
        beam_view = NULL;
    else {
        int mode, lane;

        memset(beam_view, 0, sizeof(*beam_view));
        beam_view->fir_enable = 1;
        saved_mix_settings(&mode, &lane);
        beam_view->mix_mode = (unsigned int)mode;
        beam_view->mix_lane = lane;
    }
    listen_fd = le_adapter_listen(config.socket_path);
    if (listen_fd < 0) {
        le_log_perr("unable to listen on %s", config.socket_path);
        return 1;
    }
    le_log_info("microphone daemon listening on %s (capture idle)",
                config.socket_path);


    while (running) {
        struct pollfd descriptor;
        int client_fd;
        char message[LE_ADAPTER_MSG_MAX];
        char command[64];
        char status[LE_ADAPTER_MSG_MAX];
        char *args = NULL;
        unsigned long id = 0;

        if (child_changed || stream_worker > 0) {
            int child_status;
            pid_t result;
            child_changed = 0;
            result = waitpid(stream_worker, &child_status, WNOHANG);
            if (result == stream_worker) {
                /* A worker that died mid-write must not leave stale beams. */
                if (beam_view) {
                    beam_view->running = 0;
                    beam_view->seq = (beam_view->seq + 1U) & ~1U;
                }
                le_log_info("raw microphone stream stopped (%s %d)",
                            WIFSIGNALED(child_status) ? "signal"
                            : WIFEXITED(child_status) ? "exit" : "?",
                            WIFSIGNALED(child_status)
                                ? WTERMSIG(child_status)
                                : WIFEXITED(child_status)
                                    ? WEXITSTATUS(child_status) : -1);
                stream_worker = -1;
            }
        }
        descriptor.fd = listen_fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 500) <= 0)
            continue;
        client_fd = le_adapter_accept(listen_fd);
        if (client_fd < 0)
            continue;
        /* Reap dead workers before deciding the capture is busy: without
         * this a client connecting in the window between a worker's death
         * and the next loop iteration gets a spurious "already in use". */
        {
            int child_status;

            if (stream_worker > 0 &&
                waitpid(stream_worker, &child_status, WNOHANG) ==
                    stream_worker) {
                if (beam_view) {
                    beam_view->running = 0;
                    beam_view->seq = (beam_view->seq + 1U) & ~1U;
                }
                stream_worker = -1;
            }
        }
        if (read_request_line(client_fd, message, sizeof(message)) < 0 ||
            le_adapter_parse_request(message, command, sizeof(command),
                                     &args, &id) < 0) {
            (void)respond(client_fd, id, 0, "malformed request");
            close(client_fd);
            continue;
        }

        if (!strcmp(command, "status")) {
            if (format_status(status, sizeof(status), &config,
                              stream_worker > 0,
                              capture_configured) < 0)
                (void)respond(client_fd, id, 0, "status unavailable");
            else
                (void)respond(client_fd, id, 1, status);
            close(client_fd);
        } else if (!strcmp(command, "beams")) {
            if (format_beams(status, sizeof(status)) < 0)
                (void)respond(client_fd, id, 0, "beam state unavailable");
            else
                (void)respond(client_fd, id, 1, status);
            close(client_fd);
        } else if (!strcmp(command, "set_mix")) {
            int mode = mix_mode_from_json(args, "\"mode\"");

            if (mode < 0) {
                (void)respond(client_fd, id, 0,
                              "invalid microphone mix mode");
            } else if (!beam_view) {
                (void)respond(client_fd, id, 0,
                              "microphone mix is unavailable");
            } else if (mode == LE_MIC_MIX_ANTIBEAM &&
                       !beam_view->antibeam_available) {
                (void)respond(client_fd, id, 0,
                              "anti-beam needs the measured IR file "
                              "for this device model");
            } else {
                beam_view->mix_mode = (unsigned int)mode;
                le_log_info("microphone mix set to %s", le_mic_mix_name(mode));
                (void)snprintf(status, sizeof(status),
                               "{\"microphone_mix\":\"%s\"}",
                               le_mic_mix_name(mode));
                (void)respond(client_fd, id, 1, status);
            }
            close(client_fd);
        } else if (!strcmp(command, "set_lane")) {
            int lane;

            if (lane_from_json(args, "\"lane\"", &lane) < 0) {
                (void)respond(client_fd, id, 0, "invalid microphone lane");
            } else if (!beam_view) {
                (void)respond(client_fd, id, 0,
                              "microphone lane is unavailable");
            } else {
                beam_view->mix_lane = lane;
                le_log_info("microphone lane set to %d", lane);
                (void)snprintf(status, sizeof(status),
                               "{\"microphone_lane\":%d}", lane);
                (void)respond(client_fd, id, 1, status);
            }
            close(client_fd);
        } else if (!strcmp(command, "set_fir")) {
            int enable;

            if (lane_from_json(args, "\"enable\"", &enable) < 0 ||
                (enable != 0 && enable != 1)) {
                (void)respond(client_fd, id, 0,
                              "invalid fir enable flag");
            } else if (!beam_view) {
                (void)respond(client_fd, id, 0,
                              "fir gate is unavailable");
            } else {
                beam_view->fir_enable = enable;
                le_log_info("mic fir %s", enable ? "enabled" : "disabled");
                (void)snprintf(status, sizeof(status),
                               "{\"fir_enable\":%d}", enable);
                (void)respond(client_fd, id, 1, status);
            }
            close(client_fd);
        } else if (!strcmp(command, "stream_raw")) {
            int channel;
            pid_t worker;
            if (parse_channel(args, &channel) < 0) {
                (void)respond(client_fd, id, 0,
                              "invalid raw microphone channel");
                close(client_fd);
                continue;
            }
            if (access(config.pcm_path, R_OK) != 0) {
                (void)respond(client_fd, id, 0,
                              "microphone capture endpoint unavailable");
                close(client_fd);
                continue;
            }
            if (stream_worker > 0) {
                (void)respond(client_fd, id, 0,
                              "microphone capture is already in use");
                close(client_fd);
                continue;
            }
            capture_configured = configure_capture_path(&config) == 0;
            if (!capture_configured) {
                (void)respond(client_fd, id, 0,
                              "microphone mixer configuration failed");
                close(client_fd);
                continue;
            }
            worker = fork();
            if (worker < 0) {
                (void)respond(client_fd, id, 0,
                              "unable to start microphone stream");
                close(client_fd);
                continue;
            }
            if (worker == 0) {
                close(listen_fd);
                run_stream_worker(client_fd, id, channel, 0, &config);
                _exit(0);
            }
            stream_worker = worker;
            close(client_fd);
            le_log_info("raw microphone stream started (lane=%d)", channel);
        } else if (!strcmp(command, "stream_mono")) {
            pid_t worker;
            if (access(config.pcm_path, R_OK) != 0) {
                (void)respond(client_fd, id, 0,
                              "microphone capture endpoint unavailable");
                close(client_fd);
                continue;
            }
            if (stream_worker > 0) {
                (void)respond(client_fd, id, 0,
                              "microphone capture is already in use");
                close(client_fd);
                continue;
            }
            capture_configured = configure_capture_path(&config) == 0;
            if (!capture_configured) {
                (void)respond(client_fd, id, 0,
                              "microphone mixer configuration failed");
                close(client_fd);
                continue;
            }
            worker = fork();
            if (worker < 0) {
                (void)respond(client_fd, id, 0,
                              "unable to start calibrated microphone stream");
                close(client_fd);
                continue;
            }
            if (worker == 0) {
                close(listen_fd);
                run_stream_worker(client_fd, id, 0, 1, &config);
                _exit(0);
            }
            stream_worker = worker;
            close(client_fd);
            le_log_info("calibrated mono microphone stream started");
        } else if (!strcmp(command, "stream_logical")) {
            (void)respond(client_fd, id, 0,
                          "logical microphone mapping is not available");
            close(client_fd);
        } else {
            (void)respond(client_fd, id, 0, "unknown command");
            close(client_fd);
        }
    }

    if (stream_worker > 0) {
        kill(stream_worker, SIGTERM);
        while (waitpid(stream_worker, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    close(listen_fd);
    unlink(config.socket_path);
    le_log_info("microphone daemon stopped");
    return 0;
}
