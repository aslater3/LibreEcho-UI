#include "adapter.h"
#include "log.h"
#include "voice_dsp.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
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

struct micd_config {
    const char *socket_path;
    const char *idme_dir;
    const char *dt_idme_dir;
    const char *pcm_path;
    const char *capture_bin;
    const char *mixer_bin;
};

static volatile sig_atomic_t running = 1;
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

static int configure_capture_path(const struct micd_config *config)
{
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
        {"ADC_A MICPGA Volume Ctrl", "40", "40"},
        {"ADC_B MICPGA Volume Ctrl", "40", "40"},
        {"ADC_C MICPGA Volume Ctrl", "40", "40"},
        {"ADC_D MICPGA Volume Ctrl", "40", "40"},
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
    return 0;
}

static int format_status(char *buffer, size_t size,
                         const struct micd_config *config,
                         int capture_active, int capture_configured)
{
    int values[MIC_LOGICAL_CHANNELS];
    int count = read_miccal(config->idme_dir, config->dt_idme_dir, values);
    int available = access(config->pcm_path, R_OK) == 0;
    int n;

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
        "\"selected_logical_mics\":[0,3],"
        "\"polarity\":\"all-positive-measured\","
        "\"applied_to_raw_stream\":false,"
        "\"applied_to_calibrated_stream\":true,"
        "\"mapping_available\":true,"
        "\"mapping\":\"identity-provisional; lanes 0 and 3 measured\","
        "\"relative_delay_samples\":{\"0\":4,\"3\":0}},"
        "\"pipeline\":{\"stage\":\"measured-delay-and-sum\","
        "\"highpass_hz\":80,\"vad\":\"native-energy-baseline\","
        "\"aec\":false,\"beamforming\":true,\"wake_word\":false}}",
        available ? "true" : "false",
        capture_active ? "true" : "false",
        capture_active ? "\"raw-stream\"" : "null",
        capture_configured ? "true" : "false",
        values[0], values[1], values[2], values[3],
        values[4], values[5], values[6],
        miccal_source(config->idme_dir, config->dt_idme_dir),
        count, count == MIC_LOGICAL_CHANNELS ? "true" : "false");
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
        execv(config->capture_bin, argv);
        _exit(127);
    }
}

static int relay_capture_mono(int client_fd, const struct micd_config *config)
{
    static const uint8_t identity_map[LE_VOICE_LOGICAL_CHANNELS] =
        {0, 1, 2, 3, 4, 5, 6};
    struct le_voice_calibration calibration;
    struct le_voice_beamformer beamformer;
    struct le_voice_highpass highpass;
    unsigned char input[LE_VOICE_RAW_FRAME_BYTES * 256];
    int16_t output[256];
    int miccal[LE_VOICE_LOGICAL_CHANNELS];
    size_t pending = 0;
    int pipe_fds[2];
    pid_t capture;
    ssize_t count = 0;
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
    le_voice_beamformer_init(&beamformer);
    le_voice_highpass_init(&highpass);

    while ((count = read(pipe_fds[0], input + pending,
                         sizeof(input) - pending)) > 0) {
        size_t available = pending + (size_t)count;
        size_t frames = available / LE_VOICE_RAW_FRAME_BYTES;
        size_t consumed = frames * LE_VOICE_RAW_FRAME_BYTES;

        if (frames > 0) {
            le_voice_process_beamformed_interleaved(
                input, frames, &calibration, &beamformer,
                &highpass, output);
            if (write_all(client_fd, output,
                          frames * sizeof(output[0])) < 0)
                break;
        }
        pending = available - consumed;
        if (pending > 0)
            memmove(input, input + consumed, pending);
    }
    result = count < 0 ? -1 : 0;

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
        n = snprintf(
            data, sizeof(data),
            "{\"format\":\"pcm_s16_le\",\"rate\":16000,"
            "\"channels\":1,\"bits\":16,\"valid_bits\":16,"
            "\"selected_logical_mics\":[0,3],"
            "\"calibration_applied\":true,"
            "\"calibration_mode\":\"direct_q14\","
            "\"logical_map\":\"identity-provisional\","
            "\"beamforming\":\"measured_delay_and_sum\","
            "\"relative_delay_samples\":{\"0\":4,\"3\":0},"
            "\"highpass_hz\":80}");
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
        if (write_all(client_fd, response, (size_t)n) == 0) {
            if (calibrated_mono)
                (void)relay_capture_mono(client_fd, config);
            else
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
            pid_t result = waitpid(stream_worker, &child_status, WNOHANG);
            child_changed = 0;
            if (result == stream_worker) {
                le_log_info("raw microphone stream stopped");
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
