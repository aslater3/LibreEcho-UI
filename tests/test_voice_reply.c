#include "adapter/voice_reply.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct output {
    char values[8][LE_VOICE_REPLY_SEGMENT_MAX];
    size_t count;
};

static int emit(void *context, const char *text)
{
    struct output *output = context;

    if (output->count >= 8)
        return -1;
    snprintf(output->values[output->count],
             sizeof(output->values[output->count]), "%s", text);
    ++output->count;
    return 0;
}

int main(void)
{
    struct le_voice_reply reply;
    struct output output;

    memset(&output, 0, sizeof(output));
    le_voice_reply_init(&reply, emit, &output);
    CHECK(le_voice_reply_feed(&reply, "Hello Bonnie") == 0);
    CHECK(output.count == 0);
    CHECK(le_voice_reply_feed(
              &reply, "-Mae. Your first reminder is at nine") == 0);
    CHECK(output.count == 1);
    CHECK(!strcmp(output.values[0], "Hello Bonnie-Mae."));
    CHECK(le_voice_reply_feed(&reply, " o'clock!") == 0);
    CHECK(output.count == 2);
    CHECK(!strcmp(output.values[1],
                  "Your first reminder is at nine o'clock!"));
    CHECK(le_voice_reply_feed(&reply, " Have a lovely day") == 0);
    CHECK(le_voice_reply_finish(&reply) == 0);
    CHECK(output.count == 3);
    CHECK(!strcmp(output.values[2], "Have a lovely day"));

    memset(&output, 0, sizeof(output));
    le_voice_reply_init(&reply, emit, &output);
    CHECK(le_voice_reply_feed(
              &reply,
              "Here is the quickest useful answer for your question, "
              "followed by more detail.") == 0);
    CHECK(output.count == 2);
    CHECK(!strcmp(output.values[0],
                  "Here is the quickest useful answer for your question,"));
    CHECK(!strcmp(output.values[1], "followed by more detail."));

    memset(&output, 0, sizeof(output));
    le_voice_reply_init(&reply, emit, &output);
    CHECK(le_voice_reply_feed(
              &reply,
              "This deliberately has no convenient punctuation but it "
              "still starts speaking quickly enough to meet the target.") == 0);
    CHECK(output.count == 2);
    CHECK(!strcmp(output.values[0],
                  "This deliberately has no convenient punctuation"));
    CHECK(!strcmp(output.values[1],
                  "but it still starts speaking quickly enough to meet the target."));
    puts("voice reply: streamed sentence segmentation: ok");
    return 0;
}
