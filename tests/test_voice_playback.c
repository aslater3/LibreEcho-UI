#define _POSIX_C_SOURCE 200809L

#include "adapter/voice_playback.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct output {
    char values[4][LE_VOICE_REPLY_SEGMENT_MAX];
    size_t count;
};

static int play(void *context, const char *text)
{
    struct output *output = context;
    struct timespec delay = {0, 1000000L};

    if (output->count >= 4)
        return -1;
    snprintf(output->values[output->count],
             sizeof(output->values[output->count]), "%s", text);
    ++output->count;
    nanosleep(&delay, NULL);
    return 0;
}

int main(void)
{
    struct le_voice_playback playback;
    struct output output;

    memset(&output, 0, sizeof(output));
    CHECK(le_voice_playback_start(&playback, play, &output) == 0);
    CHECK(le_voice_playback_enqueue(&playback, "First sentence.") == 0);
    CHECK(le_voice_playback_enqueue(&playback, "Second sentence.") == 0);
    CHECK(le_voice_playback_wait_idle(&playback, 1000) == 0);
    CHECK(output.count == 2);
    CHECK(!strcmp(output.values[0], "First sentence."));
    CHECK(!strcmp(output.values[1], "Second sentence."));
    le_voice_playback_stop(&playback);
    puts("voice playback: ordered asynchronous sentence queue: ok");
    return 0;
}
