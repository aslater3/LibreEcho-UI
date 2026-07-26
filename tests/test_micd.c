#include "adapter/adapter.h"

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
    char socket_path[160];
    char path[160];
    char capture_path[160];
    char capture_args_path[160];
    char mixer_path[160];
    char mixer_args_path[160];
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
    snprintf(socket_path, sizeof(socket_path), "%s/mic.sock", directory);
    snprintf(capture_path, sizeof(capture_path), "%s/fake-tinycap",
             directory);
    snprintf(capture_args_path, sizeof(capture_args_path),
             "%s/capture.args", directory);
    snprintf(mixer_path, sizeof(mixer_path), "%s/fake-tinymix",
             directory);
    snprintf(mixer_args_path, sizeof(mixer_args_path),
             "%s/mixer.args", directory);
    CHECK(mkdir(directory, 0700) == 0);
    for (i = 0; i < 7; ++i) {
        FILE *file;
        snprintf(path, sizeof(path), "%s/miccal.%d", directory, i);
        file = fopen(path, "w");
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

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        execl("./build/libreecho-micd", "libreecho-micd",
              "--foreground", "--socket", socket_path,
              "--idme-dir", directory, "--pcm-path", "/dev/null",
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
    CHECK(strstr(response, "\"capture\":{\"active\":false") != NULL);
    CHECK(strstr(response, "\"bits\":24,\"valid_bits\":16") != NULL);
    CHECK(strstr(response, "\"active_microphone_channels\":7") != NULL);
    CHECK(strstr(response, "\"inactive_transport_channels\":[7,8]") != NULL);
    CHECK(strstr(response, "\"configured\":true") != NULL);
    CHECK(strstr(response, "\"mapping_available\":true") != NULL);
    CHECK(strstr(response, "\"selected_logical_mics\":[0,3]") != NULL);
    CHECK(strstr(response,
                 "\"relative_delay_samples\":{\"0\":4,\"3\":0}") != NULL);
    CHECK(strstr(response, "\"applied_to_raw_stream\":false") != NULL);
    CHECK(strstr(response,
                 "\"applied_to_calibrated_stream\":true") != NULL);
    CHECK(strstr(response,
                 "\"stage\":\"measured-delay-and-sum\"") != NULL);
    CHECK(strstr(response, "\"beamforming\":true") != NULL);
    CHECK(strstr(response, "\"vad\":\"native-energy-baseline\"") != NULL);
    CHECK(strstr(response, "\"wake_word\":false") != NULL);
    le_adapter_close(adapter);
    adapter = le_adapter_connect(socket_path, 250);
    CHECK(adapter != NULL);
    CHECK(le_adapter_call(adapter, "stream_logical", "{}",
                          response, sizeof(response)) ==
          LE_ADAPTER_ERR_REJECTED);
    CHECK(strstr(response, "logical microphone mapping") != NULL);
    puts("micd: status exposes the measured 0/3 delay-and-sum stream");
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
        struct sockaddr_un address;
        ssize_t count;
        size_t used = 0;
        const char request[] =
            "{\"v\":1,\"id\":33,\"cmd\":\"stream_raw\","
            "\"args\":{\"channel\":2}}\n";
        const char expected_args[] =
            "--\n-D\n0\n-d\n24\n-c\n9\n-r\n16000\n-b\n24\n"
            "-p\n160\n-n\n2\n";
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
    puts("micd: raw stream uses the hardware-valid 10 ms period");
    {
        FILE *file = fopen(mixer_args_path, "r");
        size_t used;

        CHECK(file != NULL);
        used = fread(response, 1, sizeof(response) - 1, file);
        CHECK(fclose(file) == 0);
        response[used] = '\0';
        CHECK(strstr(response,
              "ADC_A Left Ip Select ADC_A DIF1_L switch\n1\n") != NULL);
        CHECK(strstr(response,
              "ADC_D Right Ip Select ADC_D DIF1_R switch\n1\n") != NULL);
        CHECK(strstr(response,
              "ADC_D MICPGA Volume Ctrl\n40\n40\n") != NULL);
        CHECK(strstr(response, "SpiTimeStamps\n0\n") != NULL);
    }
    puts("micd: stock Radar ADC profile is applied before capture");

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
    unlink(socket_path);
    for (i = 0; i < 7; ++i) {
        snprintf(path, sizeof(path), "%s/miccal.%d", directory, i);
        unlink(path);
    }
    rmdir(directory);
    return result;
}
