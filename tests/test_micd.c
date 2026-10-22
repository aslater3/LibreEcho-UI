#include "adapter/adapter.h"
#include "adapter/mic_array_render.h"

#include <math.h>
#include <poll.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Seconds of directional capture the beam test feeds micd. */
#define BEAM_TEST_SECONDS 3
#define BEAM_TEST_AZIMUTH 90.0

/*
 * Writes BEAM_TEST_SECONDS of nine-lane 24-bit capture holding broadband
 * bursts from BEAM_TEST_AZIMUTH, as the array would hear them.
 */
static int write_directional_capture(const char *path)
{
    static struct le_array_render render;
    unsigned int seed = 12345;
    FILE *file = fopen(path, "wb");
    long i;

    if (!file)
        return -1;
    le_array_render_init(&render, BEAM_TEST_AZIMUTH);
    for (i = 0; i < 16000L * BEAM_TEST_SECONDS; ++i) {
        unsigned char frame[27];
        int on = (i % 9600) < 4000;
        unsigned int c;

        seed = seed * 1103515245U + 12345U;
        le_array_render_push(&render,
                             on ? (float)((int)((seed >> 16) % 12001) - 6000)
                                : 0.0f);
        for (c = 0; c < 9; ++c) {
            long v = 0;
            if (c < LE_ARRAY_RENDER_MICS) {
                seed = seed * 1103515245U + 12345U;
                v = lrintf(le_array_render_lane(&render, c)) +
                    (long)((seed >> 16) % 41) - 20;
            }
            v *= 256; /* 16 valid bits at the top of the 24-bit word */
            frame[c * 3] = (unsigned char)(v & 0xff);
            frame[c * 3 + 1] = (unsigned char)((v >> 8) & 0xff);
            frame[c * 3 + 2] = (unsigned char)((v >> 16) & 0xff);
        }
        if (fwrite(frame, 1, sizeof(frame), file) != sizeof(frame)) {
            fclose(file);
            return -1;
        }
    }
    return fclose(file);
}

static double azimuth_error(double a, double b)
{
    double e = fmod(fabs(a - b), 360.0);
    return e > 180.0 ? 360.0 - e : e;
}

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        result = 1; \
        goto cleanup; \
    } \
} while (0)

int main(void)
{
    char directory[128];
    char dt_directory[192];
    char socket_path[160];
    char path[160];
    char capture_path[160];
    char capture_args_path[160];
    char mixer_path[160];
    char mixer_args_path[160];
    char config_path[160];
    char beam_capture_path[160];
    const int expected[7] = {16483, 16746, 17517, 17039,
                             16218, 16339, 14777};
    struct le_adapter *adapter = NULL;
    struct timespec pause_time = {0, 20000000};
    char response[LE_ADAPTER_MSG_MAX];
    pid_t child = -1;
    int result = 0;
    int stream_fd = -1;
    int i;

    snprintf(directory, sizeof(directory), "/tmp/libreecho-micd-test-%ld",
             (long)getpid());
    snprintf(dt_directory, sizeof(dt_directory), "%s/dt-idme", directory);
    snprintf(socket_path, sizeof(socket_path), "%s/mic.sock", directory);
    snprintf(capture_path, sizeof(capture_path), "%s/fake-tinycap",
             directory);
    snprintf(capture_args_path, sizeof(capture_args_path),
             "%s/capture.args", directory);
    snprintf(mixer_path, sizeof(mixer_path), "%s/fake-tinymix",
             directory);
    snprintf(mixer_args_path, sizeof(mixer_args_path),
             "%s/mixer.args", directory);
    snprintf(config_path, sizeof(config_path), "%s/web-config.json",
             directory);
    snprintf(beam_capture_path, sizeof(beam_capture_path),
             "%s/beam-capture.s24", directory);
    CHECK(mkdir(directory, 0700) == 0);
    CHECK(mkdir(dt_directory, 0700) == 0);
    for (i = 0; i < 7; ++i) {
        FILE *file;
        char node[256];
        char value_path[320];

        snprintf(node, sizeof(node), "%s/miccal.%d", dt_directory, i);
        CHECK(mkdir(node, 0700) == 0);
        snprintf(value_path, sizeof(value_path), "%s/value", node);
        snprintf(path, sizeof(path), "%s/miccal.%d", directory, i);
        file = fopen(path, "w");
        CHECK(file != NULL);
        CHECK(fprintf(file, "%d\n", expected[i]) > 0);
        CHECK(fclose(file) == 0);
        unlink(path);
        file = fopen(value_path, "w");
        CHECK(file != NULL);
        CHECK(fprintf(file, "%d\n", expected[i]) > 0);
        CHECK(fclose(file) == 0);
    }
    {
        FILE *file = fopen(capture_path, "w");
        CHECK(file != NULL);
        CHECK(fprintf(file,
              "#!/bin/sh\n"
              "printf '%%s\\n' \"$@\" > '%s'\n"
              "printf 'PCM'\n",
              capture_args_path) > 0);
        CHECK(fclose(file) == 0);
        CHECK(chmod(capture_path, 0700) == 0);
    }
    {
        FILE *file = fopen(mixer_path, "w");
        CHECK(file != NULL);
        CHECK(fprintf(file,
              "#!/bin/sh\n"
              "printf '%%s\\n' \"$@\" >> '%s'\n",
              mixer_args_path) > 0);
        CHECK(fclose(file) == 0);
        CHECK(chmod(mixer_path, 0700) == 0);
    }

    {
        FILE *file = fopen(config_path, "w");
        CHECK(file != NULL);
        CHECK(fprintf(file, "{\"volume\": 40, \"microphone_gain\": 50}\n") > 0);
        CHECK(fclose(file) == 0);
    }

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        /* A stale boot-time gain must not win over the saved one. */
        if (setenv("LE_MIC_CAPTURE_GAIN", "65", 1) ||
            setenv("LE_MIC_CONFIG", config_path, 1))
            _exit(127);
        execl("./build/libreecho-micd", "libreecho-micd",
              "--foreground", "--socket", socket_path,
              "--idme-dir", "/missing-idme", "--dt-idme-dir", dt_directory,
              "--pcm-path", "/dev/null",
              "--capture-bin", capture_path,
              "--mixer-bin", mixer_path, (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 100; ++i) {
        adapter = le_adapter_connect(socket_path, 250);
        if (adapter)
            break;
        nanosleep(&pause_time, NULL);
    }
    CHECK(adapter != NULL);
    CHECK(le_adapter_call(adapter, "status", "{}",
                          response, sizeof(response)) == LE_ADAPTER_OK);
    CHECK(strstr(response,
          "\"q14\":[16483,16746,17517,17039,16218,16339,14777]") != NULL);
    CHECK(strstr(response,
          "\"source\":\"/tmp/libreecho-micd-test-") != NULL);
    CHECK(strstr(response, "/dt-idme/miccal.N/value\"") != NULL);
    CHECK(strstr(response, "\"capture\":{\"active\":false") != NULL);
    CHECK(strstr(response, "\"bits\":24,\"valid_bits\":16") != NULL);
    CHECK(strstr(response, "\"active_microphone_channels\":7") != NULL);
    CHECK(strstr(response, "\"inactive_transport_channels\":[7,8]") != NULL);
    CHECK(strstr(response, "\"configured\":true") != NULL);
    CHECK(strstr(response, "\"mapping_available\":true") != NULL);
    CHECK(strstr(response, "\"selected_logical_mics\":[0,1,3,4]") != NULL);
    CHECK(strstr(response,
                 "\"relative_delay_samples\":{}") != NULL);
    CHECK(strstr(response, "\"applied_to_raw_stream\":false") != NULL);
    CHECK(strstr(response,
                 "\"applied_to_calibrated_stream\":true") != NULL);
    CHECK(strstr(response,
                 "\"stage\":\"fixed-beam-select\"") != NULL);
    CHECK(strstr(response, "\"beamforming\":true") != NULL);
    CHECK(strstr(response,
                 "\"ring_azimuth_deg\":[150,210,270,330,30,90]") != NULL);
    CHECK(strstr(response, "\"centre_lane\":6,\"ring_radius_mm\":36") != NULL);
    CHECK(strstr(response, "\"vad\":\"native-energy-baseline\"") != NULL);
    CHECK(strstr(response, "\"wake_word\":false") != NULL);
    le_adapter_close(adapter);
    adapter = le_adapter_connect(socket_path, 250);
    CHECK(adapter != NULL);
    CHECK(le_adapter_call(adapter, "stream_logical", "{}",
                          response, sizeof(response)) ==
          LE_ADAPTER_ERR_REJECTED);
    CHECK(strstr(response, "logical microphone mapping") != NULL);
    puts("micd: status exposes the measured geometry and fixed-beam pipeline");
    le_adapter_close(adapter);
    adapter = le_adapter_connect(socket_path, 250);
    CHECK(adapter != NULL);
    CHECK(le_adapter_call(adapter, "beams", "{}",
                          response, sizeof(response)) == LE_ADAPTER_OK);
    CHECK(strstr(response, "\"running\":false") != NULL);
    le_adapter_close(adapter);
    adapter = NULL;
    adapter = le_adapter_connect(socket_path, 250);
    CHECK(adapter != NULL);
    CHECK(le_adapter_call(adapter, "stream_raw", "{\"channel\":7}",
                          response, sizeof(response)) ==
          LE_ADAPTER_ERR_REJECTED);
    CHECK(strstr(response, "invalid raw microphone channel") != NULL);
    le_adapter_close(adapter);
    adapter = NULL;
    {
        /* The owner changes the gain after micd started. */
        FILE *file = fopen(config_path, "w");
        CHECK(file != NULL);
        CHECK(fprintf(file, "{\"microphone_gain\":85,\"volume\":40}\n") > 0);
        CHECK(fclose(file) == 0);
    }
    {
        struct sockaddr_un address;
        ssize_t count;
        size_t used = 0;
        const char request[] =
            "{\"v\":1,\"id\":33,\"cmd\":\"stream_raw\","
            "\"args\":{\"channel\":2}}\n";
        const char expected_args[] =
            "--\n-D\n0\n-d\n24\n-c\n9\n-r\n16000\n-b\n24\n"
            "-p\n640\n-n\n4\n";
        FILE *file;

        stream_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(stream_fd >= 0);
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, socket_path,
                sizeof(address.sun_path) - 1);
        CHECK(connect(stream_fd, (struct sockaddr *)&address,
                      sizeof(address)) == 0);
        CHECK(write(stream_fd, request, sizeof(request) - 1) ==
              (ssize_t)(sizeof(request) - 1));
        while (used + 1 < sizeof(response) &&
               (count = read(stream_fd, response + used,
                             sizeof(response) - used - 1)) > 0)
            used += (size_t)count;
        response[used] = '\0';
        CHECK(strstr(response, "\"ok\":true") != NULL);
        CHECK(strstr(response, "\"selected_raw_channel\":2") != NULL);
        CHECK(strstr(response, "\"bits\":24,\"valid_bits\":16") != NULL);
        CHECK(strstr(response, "PCM") != NULL);
        close(stream_fd);
        stream_fd = -1;
        for (i = 0; i < 100 && access(capture_args_path, R_OK); ++i)
            nanosleep(&pause_time, NULL);
        file = fopen(capture_args_path, "r");
        CHECK(file != NULL);
        used = fread(response, 1, sizeof(response) - 1, file);
        CHECK(fclose(file) == 0);
        response[used] = '\0';
        CHECK(strcmp(response, expected_args) == 0);
    }
    puts("micd: raw stream uses the full-ring 40 ms period");
    {
        FILE *file = fopen(mixer_args_path, "r");
        size_t used;

        CHECK(file != NULL);
        used = fread(response, 1, sizeof(response) - 1, file);
        CHECK(fclose(file) == 0);
        response[used] = '\0';
        CHECK(strstr(response,
              "Mic PGA Switch\n1\n1\n") != NULL);
        CHECK(strstr(response,
              "ADCFGA Left Mute Switch\n0\n") != NULL);
        CHECK(strstr(response,
              "ADCFGA Right Mute Switch\n0\n") != NULL);
        CHECK(strstr(response,
              "ADC_A Left Ip Select ADC_A DIF1_L switch\n1\n") != NULL);
        CHECK(strstr(response,
              "ADC_D Right Ip Select ADC_D DIF1_R switch\n1\n") != NULL);
        CHECK(strstr(response,
              "ADC_A DIF1_L Input Gain\n1\n") != NULL);
        CHECK(strstr(response,
              "ADC_D DIF1_R Input Gain\n1\n") != NULL);
        CHECK(strstr(response,
              "ADC_D MICPGA Volume Ctrl\n40\n40\n") != NULL);
        CHECK(strstr(response, "SpiTimeStamps\n0\n") != NULL);
        CHECK(strstr(response,
              "ADC_D MICPGA Volume Ctrl\n68\n68\n") != NULL);
        CHECK(strstr(response,
              "ADC_D MICPGA Volume Ctrl\n52\n52\n") == NULL);
    }
    puts("micd: stock Radar ADC profile is applied before capture");
    {
        /*
         * Beam view: a calibrated mono stream fed sound from one direction
         * must report that direction through "beams" while it runs, and
         * report nothing once it stops.
         */
        struct sockaddr_un address;
        const char request[] =
            "{\"v\":1,\"id\":34,\"cmd\":\"stream_mono\",\"args\":{}}\n";
        char sink[4096];
        double doa = -1.0;
        int running_seen = 0;
        FILE *file;

        CHECK(write_directional_capture(beam_capture_path) == 0);
        file = fopen(capture_path, "w");
        CHECK(file != NULL);
        CHECK(fprintf(file, "#!/bin/sh\ncat '%s'\nsleep 2\n",
                      beam_capture_path) > 0);
        CHECK(fclose(file) == 0);
        for (i = 0; i < 100; ++i) {
            /* The raw stream worker from the previous check must be gone. */
            adapter = le_adapter_connect(socket_path, 250);
            CHECK(adapter != NULL);
            CHECK(le_adapter_call(adapter, "status", "{}", response,
                                  sizeof(response)) == LE_ADAPTER_OK);
            le_adapter_close(adapter);
            adapter = NULL;
            if (strstr(response, "\"capture\":{\"active\":false"))
                break;
            nanosleep(&pause_time, NULL);
        }
        stream_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(stream_fd >= 0);
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
        CHECK(connect(stream_fd, (struct sockaddr *)&address,
                      sizeof(address)) == 0);
        CHECK(write(stream_fd, request, sizeof(request) - 1) ==
              (ssize_t)(sizeof(request) - 1));
        for (i = 0; i < 200; ++i) {
            struct pollfd descriptor = {stream_fd, POLLIN, 0};
            const char *field;

            /* Drain the mono audio so the worker never blocks on us. */
            while (poll(&descriptor, 1, 5) > 0 &&
                   read(stream_fd, sink, sizeof(sink)) > 0)
                ;
            adapter = le_adapter_connect(socket_path, 250);
            CHECK(adapter != NULL);
            CHECK(le_adapter_call(adapter, "beams", "{}", response,
                                  sizeof(response)) == LE_ADAPTER_OK);
            le_adapter_close(adapter);
            adapter = NULL;
            field = strstr(response, "\"hops\":");
            if (strstr(response, "\"running\":true") && field &&
                strtol(field + 7, NULL, 10) >
                    (long)(16000L * BEAM_TEST_SECONDS / 128 - 4)) {
                running_seen = 1;
                field = strstr(response, "\"doa_deg\":");
                CHECK(field != NULL);
                doa = strtod(field + 10, NULL);
                CHECK(strstr(response, "\"beams\":[{\"deg\":0,") != NULL);
                break;
            }
            nanosleep(&pause_time, NULL);
        }
        CHECK(running_seen);
        if (azimuth_error(doa, BEAM_TEST_AZIMUTH) > 20.0)
            fprintf(stderr, "micd: beams reported %.1f deg for a source at "
                    "%.0f deg: %s\n", doa, BEAM_TEST_AZIMUTH, response);
        CHECK(azimuth_error(doa, BEAM_TEST_AZIMUTH) <= 20.0);
        close(stream_fd);
        stream_fd = -1;
        for (i = 0; i < 250; ++i) {
            adapter = le_adapter_connect(socket_path, 250);
            CHECK(adapter != NULL);
            CHECK(le_adapter_call(adapter, "beams", "{}", response,
                                  sizeof(response)) == LE_ADAPTER_OK);
            le_adapter_close(adapter);
            adapter = NULL;
            if (strstr(response, "\"running\":false"))
                break;
            nanosleep(&pause_time, NULL);
        }
        CHECK(strstr(response, "\"running\":false") != NULL);
        printf("micd: beams report a source at %.0f deg as %.1f deg, "
               "and clear when the stream stops\n", BEAM_TEST_AZIMUTH, doa);
    }
    puts("micd: capture start applies the gain saved after boot");

cleanup:
    if (stream_fd >= 0)
        close(stream_fd);
    if (adapter)
        le_adapter_close(adapter);
    if (child > 0) {
        kill(child, SIGTERM);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    unlink(capture_args_path);
    unlink(capture_path);
    unlink(mixer_args_path);
    unlink(mixer_path);
    unlink(config_path);
    unlink(beam_capture_path);
    unlink(socket_path);
    for (i = 0; i < 7; ++i) {
        snprintf(path, sizeof(path), "%s/miccal.%d", directory, i);
        unlink(path);
        {
            char node[256];
            char value_path[320];
            snprintf(node, sizeof(node), "%s/miccal.%d", dt_directory, i);
            snprintf(value_path, sizeof(value_path), "%s/value", node);
            unlink(value_path);
            rmdir(node);
        }
    }
    rmdir(dt_directory);
    rmdir(directory);
    return result;
}
