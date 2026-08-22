/*
 * libreecho-capture-mux -- the capture binary micd spawns, with the ability to
 * substitute recorded audio for the microphones without interrupting anything.
 *
 * Why this exists as a mux rather than as a source switch in micd:
 *
 *   waked connects to micd's mono stream exactly once and has no reconnect
 *   path, and its init script has no supervisor. If the capture stream ever
 *   ends, the wake word is gone until something restarts waked by hand. So
 *   injection must never break the stream -- which rules out stopping the
 *   capture child and starting a different one.
 *
 * This process therefore stays resident for the life of the stream. It reads
 * the real capture binary continuously and forwards it, and when an injection
 * is requested it emits synthesised frames in its place for the duration of
 * the recording, then goes back to forwarding. Its stdout is never closed and
 * the cadence never changes, so micd and waked cannot tell the difference.
 *
 * Control is a file drop, so no IPC and no micd change:
 *   <control-dir>/pending.raw   16 kHz mono S16LE; renamed to active.raw when
 *                               picked up, removed when finished
 * Everything lives in /run (tmpfs), so nothing survives a reboot and none of
 * it touches the /data contract.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHANNELS 9
#define PERIOD 640                  /* 40 ms at 16 kHz, matching micd */
#define RATE 16000
/*
 * micd beamforms logical mics 0 and 3 with relative_delay_samples {0:4, 3:0}:
 * it holds mic 0 back so a wavefront that reached it first lines up with mic 3.
 * Injected audio must carry that same delay or delay-and-sum turns into a comb
 * filter with its first null at rate/(2*4) = 2 kHz -- a 13.6 dB hole straight
 * through the consonant band, which makes every injected utterance score worse
 * than the same audio would in the room.
 */
#define ARRAY_DELAY 4
#define HISTORY 64

static volatile sig_atomic_t running = 1;

static void stop(int signum) { (void)signum; running = 0; }

static void put24(unsigned char *out, int16_t sample)
{
    /* The codec left-justifies 16 valid bits inside a 24-bit word. */
    int32_t value = (int32_t)sample << 8;
    out[0] = (unsigned char)(value & 0xff);
    out[1] = (unsigned char)((value >> 8) & 0xff);
    out[2] = (unsigned char)((value >> 16) & 0xff);
}

static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

static pid_t spawn_capture(const char *bin, int *out_fd)
{
    int fds[2];
    pid_t child;

    if (pipe(fds) < 0)
        return -1;
    child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (child == 0) {
        char *const argv[] = {
            (char *)bin, (char *)"-D", (char *)"0", (char *)"-d", (char *)"24",
            (char *)"-c", (char *)"9", (char *)"-r", (char *)"16000",
            (char *)"-b", (char *)"24", (char *)"-p", (char *)"640",
            (char *)"-n", (char *)"4", NULL
        };
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(fds[1]);
        execv(bin, argv);
        _exit(127);
    }
    close(fds[1]);
    *out_fd = fds[0];
    return child;
}

static int read_full(int fd, void *buffer, size_t length)
{
    unsigned char *p = buffer;
    size_t got = 0;
    while (got < length) {
        ssize_t n = read(fd, p + got, length - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return 0;
        got += (size_t)n;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *capture_bin = "/sbin/tinycap";
    const char *control_dir = "/run/libreecho/mic-inject";
    char pending[512], active[512];
    unsigned char passthrough[PERIOD * CHANNELS * 3];
    unsigned char frame[PERIOD * CHANNELS * 3];
    int16_t mono[PERIOD];
    int16_t history[HISTORY];
    int history_pos = 0;
    FILE *injected = NULL;
    int capture_fd = -1;
    pid_t capture;
    int i;

    /*
     * Configuration comes from the environment, because micd builds this
     * process's argv itself -- it passes tinycap's flags and knows nothing
     * about a mux. Those flags are simply ignored here. The explicit options
     * exist for testing this binary directly.
     */
    {
        const char *text = getenv("LE_CAPTURE_MUX_BIN");
        if (text && *text)
            capture_bin = text;
        text = getenv("LE_CAPTURE_MUX_CONTROL_DIR");
        if (text && *text)
            control_dir = text;
    }
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--capture-bin") && i + 1 < argc)
            capture_bin = argv[++i];
        else if (!strcmp(argv[i], "--control-dir") && i + 1 < argc)
            control_dir = argv[++i];
    }
    memset(history, 0, sizeof(history));
    snprintf(pending, sizeof(pending), "%s/pending.raw", control_dir);
    snprintf(active, sizeof(active), "%s/active.raw", control_dir);
    (void)mkdir(control_dir, 0755);

    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGPIPE, SIG_IGN);

    capture = spawn_capture(capture_bin, &capture_fd);
    if (capture < 0) {
        fprintf(stderr, "capture-mux: cannot start %s\n", capture_bin);
        return 1;
    }

    while (running) {
        int have = read_full(capture_fd, passthrough, sizeof(passthrough));
        if (have <= 0)
            break;              /* the real capture ended; so must we */

        /*
         * Pick up a new injection between periods. Renaming rather than
         * opening in place means a half-written upload can never be played:
         * the API writes to a temporary name and renames it into pending.raw
         * only once it is complete.
         */
        if (!injected && rename(pending, active) == 0) {
            injected = fopen(active, "rb");
            if (!injected)
                (void)unlink(active);
        }

        if (injected) {
            size_t got = fread(mono, sizeof(int16_t), PERIOD, injected);
            if (got == 0) {
                fclose(injected);
                injected = NULL;
                (void)unlink(active);
                /* fall through and forward this period from the codec */
            } else {
                size_t s;
                for (s = got; s < PERIOD; ++s)
                    mono[s] = 0;
                for (s = 0; s < PERIOD; ++s) {
                    int16_t sample = mono[s];
                    int16_t lagged;
                    int c;

                    history[history_pos] = sample;
                    lagged = history[(history_pos + HISTORY - ARRAY_DELAY) % HISTORY];
                    history_pos = (history_pos + 1) % HISTORY;
                    for (c = 0; c < CHANNELS; ++c)
                        put24(frame + (s * CHANNELS + (size_t)c) * 3,
                              c == 3 ? lagged : sample);
                }
                if (write_all(STDOUT_FILENO, frame, sizeof(frame)) < 0)
                    break;
                continue;
            }
        }
        if (write_all(STDOUT_FILENO, passthrough, sizeof(passthrough)) < 0)
            break;
    }

    if (injected) {
        fclose(injected);
        (void)unlink(active);
    }
    kill(capture, SIGTERM);
    while (waitpid(capture, NULL, 0) < 0 && errno == EINTR)
        ;
    close(capture_fd);
    return 0;
}
