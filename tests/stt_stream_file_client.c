#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint16_t get_u16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int read_all(FILE *file, void *buffer, size_t size)
{
    return fread(buffer, 1, size, file) == size ? 0 : -1;
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *p = buffer;

    while (size) {
        ssize_t count = write(fd, p, size);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        p += count;
        size -= (size_t)count;
    }
    return 0;
}

static int read_line(int fd, char *buffer, size_t size)
{
    size_t used = 0;

    while (used + 1 < size) {
        ssize_t count = read(fd, buffer + used, 1);

        if (count < 0 && errno == EINTR)
            continue;
        if (count != 1)
            return -1;
        if (buffer[used++] == '\n') {
            buffer[used] = '\0';
            return 0;
        }
    }
    return -1;
}

static int connect_socket(const char *path)
{
    struct sockaddr_un address;
    int fd;

    if (strlen(path) >= sizeof(address.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    unsigned char riff[12];
    unsigned char chunk[8];
    unsigned char format[40];
    unsigned char buffer[4096];
    char line[8192];
    FILE *wav;
    uint32_t data_size = 0;
    int have_format = 0;
    int realtime = 0;
    int saw_final = 0;
    int fd;
    pid_t writer;

    if (argc != 3 && argc != 4) {
        fprintf(stderr,
                "usage: %s SOCKET PCM16_MONO_16KHZ_WAV [--realtime]\n",
                argv[0]);
        return 2;
    }
    if (argc == 4) {
        if (strcmp(argv[3], "--realtime"))
            return 2;
        realtime = 1;
    }
    wav = fopen(argv[2], "rb");
    if (!wav)
        return 1;
    if (read_all(wav, riff, sizeof(riff)) < 0 ||
        memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4))
        goto fail;
    while (read_all(wav, chunk, sizeof(chunk)) == 0) {
        uint32_t size = get_u32(chunk + 4);

        if (!memcmp(chunk, "fmt ", 4)) {
            size_t reading = size < sizeof(format) ? size : sizeof(format);

            if (reading < 16 || read_all(wav, format, reading) < 0)
                goto fail;
            if (size > reading &&
                fseek(wav, (long)(size - reading), SEEK_CUR) != 0)
                goto fail;
            if (get_u16(format) != 1 || get_u16(format + 2) != 1 ||
                get_u32(format + 4) != 16000 ||
                get_u16(format + 14) != 16)
                goto fail;
            have_format = 1;
        } else if (!memcmp(chunk, "data", 4)) {
            data_size = size;
            break;
        } else if (fseek(wav, (long)size, SEEK_CUR) != 0) {
            goto fail;
        }
        if (size & 1U)
            (void)fseek(wav, 1, SEEK_CUR);
    }
    if (!have_format || !data_size)
        goto fail;

    fd = connect_socket(argv[1]);
    if (fd < 0)
        goto fail;
    if (write_all(
            fd,
            "{\"v\":1,\"id\":1,\"cmd\":\"recognize_stream\","
            "\"args\":{}}\n",
            strlen(
                "{\"v\":1,\"id\":1,\"cmd\":\"recognize_stream\","
                "\"args\":{}}\n")) < 0 ||
        read_line(fd, line, sizeof(line)) < 0)
        goto fail_socket;
    fputs(line, stdout);
    writer = fork();
    if (writer < 0)
        goto fail_socket;
    if (writer == 0) {
        signal(SIGPIPE, SIG_IGN);
        while (data_size) {
            size_t reading =
                data_size < sizeof(buffer) ? data_size : sizeof(buffer);

            if (fread(buffer, 1, reading, wav) != reading ||
                write_all(fd, buffer, reading) < 0)
                _exit(0);
            data_size -= (uint32_t)reading;
            if (realtime) {
                struct timespec delay;
                uint64_t nanoseconds =
                    (uint64_t)reading * 1000000000ULL / 32000ULL;

                delay.tv_sec = (time_t)(nanoseconds / 1000000000ULL);
                delay.tv_nsec = (long)(nanoseconds % 1000000000ULL);
                while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
                    ;
            }
        }
        (void)shutdown(fd, SHUT_WR);
        _exit(0);
    }
    while (read_line(fd, line, sizeof(line)) == 0) {
        fputs(line, stdout);
        if (strstr(line, "\"event\":\"transcript\""))
            saw_final = 1;
    }
    (void)waitpid(writer, NULL, 0);
    close(fd);
    fclose(wav);
    return saw_final ? 0 : 1;

fail_socket:
    close(fd);
fail:
    fclose(wav);
    return 1;
}
