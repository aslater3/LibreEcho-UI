/* NQPTP readiness: the bound control port, not /dev/shm/nqptp (gone in 1.2.9). */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#define main airplayd_program_main
#include "../src/adapter/airplayd.c"
#undef main

#define HEADER "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n"
/* Lines as /proc/net/udp shows them on a radar running nqptp 1.2.9. */
#define NQPTP_LOOPBACK "   17: 0100007F:2328 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 48060 2 3fc666a2 0\n"
#define NQPTP_ANY "   18: 00000000:2328 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 48061 2 3fc666a3 0\n"
#define PTP_EVENT "   19: 00000000:013F 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 48062 2 3fc666a4 0\n"
#define OTHER_HOST "   20: 0A00000A:2328 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 48063 2 3fc666a5 0\n"

static void table(const char *path, const char *body)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

int main(void)
{
    char dir[] = "/tmp/le-nqptp-ready-XXXXXX", path[128];
    struct airplay_ctx ctx;
    pid_t child;
    int status;

    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/udp", dir);

    table(path, HEADER NQPTP_LOOPBACK);
    assert(nqptp_control_bound(path));
    table(path, HEADER PTP_EVENT NQPTP_ANY);
    assert(nqptp_control_bound(path));
    table(path, HEADER PTP_EVENT);
    assert(!nqptp_control_bound(path));            /* PTP ports alone are not the control port */
    table(path, HEADER OTHER_HOST);
    assert(!nqptp_control_bound(path));            /* not loopback or any */
    table(path, HEADER);
    assert(!nqptp_control_bound(path));
    assert(!nqptp_control_bound("/nonexistent/le-udp"));

    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.nqptp_udp_table, sizeof(ctx.nqptp_udp_table), "%s", path);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        pause();
        _exit(0);
    }
    ctx.nqptp_pid = child;
    /* No /dev/shm/nqptp anywhere: a bound control port is enough. */
    table(path, HEADER NQPTP_LOOPBACK);
    assert(wait_for_nqptp(&ctx, 3));
    table(path, HEADER PTP_EVENT);
    assert(!wait_for_nqptp(&ctx, 3));
    kill(child, SIGKILL);
    assert(waitpid(child, &status, 0) == child);
    ctx.nqptp_pid = child;                          /* reaped: no longer alive */
    table(path, HEADER NQPTP_LOOPBACK);
    assert(!wait_for_nqptp(&ctx, 3));
    ctx.nqptp_pid = -1;
    assert(!wait_for_nqptp(&ctx, 3));

    unlink(path);
    rmdir(dir);
    puts("AirPlay NQPTP control-port readiness: PASS");
    return 0;
}
