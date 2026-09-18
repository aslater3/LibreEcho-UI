#define _POSIX_C_SOURCE 200809L
#define LE_INIT_AGENTD "/tmp/libreecho-pr152-agentd.init"
#define LE_INIT_STTD "/tmp/libreecho-pr152-sttd.init"
#define LE_INIT_TTSD "/tmp/libreecho-pr152-ttsd.init"
#define LE_INIT_WYOMINGD "/tmp/libreecho-pr152-wyomingd.init"
#define static
#include "../src/api.c"
#undef static

#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *const init_paths[] = {
    LE_INIT_AGENTD, LE_INIT_STTD, LE_INIT_TTSD, LE_INIT_WYOMINGD
};

static void write_init_script(const char *path, int status)
{
    FILE *file = fopen(path, "w");

    assert(file != NULL);
    assert(fputs("#!/bin/sh\nsleep 1\nexit ", file) >= 0);
    assert(fprintf(file, "%d\n", status) > 0);
    assert(fclose(file) == 0);
    assert(chmod(path, 0700) == 0);
}

static void remove_scripts(void)
{
    size_t i;

    for (i = 0; i < sizeof(init_paths) / sizeof(init_paths[0]); ++i)
        unlink(init_paths[i]);
}

static void wait_for_restart(void)
{
    struct timespec delay = {0, 50000000L};
    unsigned int attempts;

    for (attempts = 0; attempts < 240; ++attempts) {
        if (!voice_pipeline_restart_pending())
            return;
        nanosleep(&delay, NULL);
    }
    assert(!"voice pipeline restart did not finish");
}

int main(void)
{
    size_t i;

    remove_scripts();
    for (i = 0; i < sizeof(init_paths) / sizeof(init_paths[0]); ++i)
        write_init_script(init_paths[i], 0);

    /* One restart is accepted; a second cannot create another worker. */
    assert(apply_voice_pipeline_mode("local") == LE_BUSY);
    assert(apply_voice_pipeline_mode("local") == LE_BUSY);
    wait_for_restart();
    assert(!strcmp(voice_pipeline_restart_state(), "ready"));

    /* A child failure is observable and is returned on the next PUT attempt. */
    write_init_script(LE_INIT_AGENTD, 1);
    assert(apply_voice_pipeline_mode("local") == LE_BUSY);
    wait_for_restart();
    assert(!strcmp(voice_pipeline_restart_state(), "failed"));
    assert(apply_voice_pipeline_mode("local") == LE_IO);

    /* Home Assistant is rejected before any local daemon is stopped. */
    unlink(LE_INIT_WYOMINGD);
    assert(apply_voice_pipeline_mode("home-assistant") == LE_NOT_SUPPORTED);

    remove_scripts();
    puts("voice pipeline restart: bounded, observable, and capability-gated PASS");
    return 0;
}
