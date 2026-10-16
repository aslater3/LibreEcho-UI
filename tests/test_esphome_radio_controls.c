/* Actual fork/signal/wait lifecycle on a private progress pipe; no media IO. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
static pid_t fixture_fork(void);
static int fixture_kill(pid_t pid, int sig);
static int fixture_open(const char *path, int flags, ...);
static int fixture_openat(int dir, const char *path, int flags, ...);
#define fork fixture_fork
#define kill fixture_kill
#define open fixture_open
#define openat fixture_openat
#define main radiod_program_main
#include "../src/adapter/radiod.c"
#undef main
#undef fork
#undef kill
#undef open
#undef openat

static int progress[2], fail_fork, fail_signal, ignore_term, signal_calls;
static pid_t fixture_fork(void)
{
    pid_t child;
    if (fail_fork) { errno = EAGAIN; return -1; }
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        unsigned int position = 0;
        close(progress[0]);
        assert(player_parent_guard() == 0);
        signal(SIGTERM, ignore_term ? SIG_IGN : SIG_DFL);
        for (;;) {
            ++position;
            if (write(progress[1], &position, sizeof(position)) != sizeof(position)) _exit(0);
            delay_ms(5);
        }
    }
    return child;
}
static int fixture_kill(pid_t pid, int sig)
{
    ++signal_calls;
    if (fail_signal) { errno = EPERM; return -1; }
    return kill(pid, sig);
}
static int fixture_open(const char *path, int flags, ...)
{ (void)path; (void)flags; assert(0 && "no host file/media access"); return -1; }
static int fixture_openat(int dir, const char *path, int flags, ...)
{ (void)dir; (void)path; (void)flags; assert(0 && "no host USB access"); return -1; }
static char response[4096];
static void command(const char *cmd, int ok)
{
    char request[1024];
    snprintf(request, sizeof(request), "{\"v\":1,\"id\":8,\"cmd\":\"%s\",\"args\":{\"url\":\"http://fixture.invalid/radio\"}}", cmd);
    assert(handle(request, response, sizeof(response), "fixture-bus") > 0);
    if (!strstr(response, ok ? "\"ok\":true" : "\"ok\":false")) {
        fprintf(stderr, "%s -> %s", cmd, response); assert(0 && "radio command result");
    }
}
static unsigned int read_progress(void)
{
    struct pollfd fd = {progress[0], POLLIN, 0};
    unsigned int value;
    assert(poll(&fd, 1, 1000) == 1);
    assert(read(progress[0], &value, sizeof(value)) == sizeof(value));
    return value;
}
static unsigned int drain_progress(void)
{
    struct pollfd fd = {progress[0], POLLIN, 0};
    unsigned int last = 0;
    while (poll(&fd, 1, 0) == 1) last = read_progress();
    return last;
}
int main(void)
{
    pid_t original, bystander;
    unsigned int last, resumed;
    int before, metadata;
    struct pollfd fd;
    struct timespec begin, end;
    long elapsed;
    signal(SIGPIPE, SIG_IGN);
    assert(pipe(progress) == 0);
    command("pause", 0); command("resume", 0);
    command("play", 1); original = player_pid; metadata = meta_fd;
    last = read_progress(); assert(last == 1);
    command("pause", 1);
    assert(player_pid == original && meta_fd == metadata);
    command("status", 1);
    assert(strstr(response, "\"playing\":false") && strstr(response, "\"paused\":true"));
    assert(strstr(response, "http://fixture.invalid/radio"));
    last = drain_progress(); /* Any queued bytes precede pause acknowledgement. */
    fd.fd = progress[0]; fd.events = POLLIN; fd.revents = 0;
    assert(poll(&fd, 1, 80) == 0); /* Worker is really frozen, not restarted. */
    command("pause", 1);
    command("resume", 1);
    resumed = read_progress(); assert(resumed > 1 && (!last || resumed == last + 1));
    assert(player_pid == original && meta_fd == metadata);
    command("status", 1); assert(strstr(response, "\"playing\":true") && strstr(response, "\"paused\":false"));
    fail_signal = 1; command("pause", 0); fail_signal = 0;
    command("status", 1); assert(strstr(response, "\"playing\":true"));
    command("pause", 1);
    fail_signal = 1; command("resume", 0); fail_signal = 0;
    command("status", 1); assert(strstr(response, "\"paused\":true"));
    fail_signal = 1; command("stop", 0); command("play", 0); fail_signal = 0;
    assert(player_pid == original && meta_fd == metadata);
    clock_gettime(CLOCK_MONOTONIC, &begin);
    command("stop", 1);
    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsed = (end.tv_sec - begin.tv_sec) * 1000 + (end.tv_nsec - begin.tv_nsec)/1000000;
    assert(elapsed < 1000 && player_pid == -1 && meta_fd == -1);
    assert(fcntl(metadata, F_GETFD) == -1 && errno == EBADF);
    assert(waitpid(original, NULL, WNOHANG) == -1 && errno == ECHILD);
    command("status", 1); assert(strstr(response, "\"paused\":false") && strstr(response, "\"url\":\"\""));
    drain_progress();
    ignore_term = 1; command("play", 1); original = player_pid; read_progress();
    command("pause", 1); clock_gettime(CLOCK_MONOTONIC, &begin);
    command("stop", 1); clock_gettime(CLOCK_MONOTONIC, &end);
    elapsed = (end.tv_sec - begin.tv_sec) * 1000 + (end.tv_nsec - begin.tv_nsec)/1000000;
    assert(elapsed < 1000 && waitpid(original, NULL, WNOHANG) == -1 && errno == ECHILD);
    ignore_term = 0; drain_progress();
    /* A stale, externally reaped PID that now names a non-child must not be signalled. */
    before = signal_calls; player_pid = getpid();
    command("pause", 0); command("stop", 1); assert(signal_calls == before);
    bystander = fork(); assert(bystander >= 0);
    if (bystander == 0) { for (;;) pause(); }
    player_pid = bystander;
    /* Bystander remains outside playback state when no stale PID names it. */
    player_pid = -1; command("stop", 1); assert(kill(bystander, 0) == 0);
    assert(kill(bystander, SIGKILL) == 0 && waitpid(bystander, NULL, 0) == bystander);
    fail_fork = 1; command("play", 0); fail_fork = 0;
    assert(player_pid == -1 && meta_fd == -1);
    command("play", 1); original = player_pid; read_progress();
    kill(original, SIGKILL); assert(waitpid(original, NULL, 0) == original);
    before = signal_calls; command("resume", 0); assert(signal_calls == before);
    command("status", 1); assert(strstr(response, "\"playing\":false"));
    close(progress[0]); close(progress[1]);
    puts("radio controls: same-worker pause/resume, bounded stopped-worker teardown, errors/stale PID isolation: ok");
    return 0;
}
