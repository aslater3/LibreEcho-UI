#include "adapter/stop_intent.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *yes[] = {"stop", "Stop!", "Alexa, please stop.",
        "Could you stop the radio please?", "stop talking", "be quiet",
        "turn the music off", "dismiss the timer", "stop the noise"};
    const char *no[] = {NULL, "", "stopwatch", "bus stop", "don't stop",
        "don’t stop", "do not stop", "please don't stop", "never stop",
        "tell me about the bus stop", "turn it off", "stop the lights",
        "play don't stop me now", "stop and play something else"};
    char oversized[400];
    size_t i;
    for (i = 0; i < sizeof(yes) / sizeof(yes[0]); ++i)
        assert(le_stop_intent_matches(yes[i]));
    for (i = 0; i < sizeof(no) / sizeof(no[0]); ++i)
        assert(!le_stop_intent_matches(no[i]));
    memset(oversized, ' ', sizeof(oversized));
    memcpy(oversized, "stop", 4);
    memcpy(oversized + sizeof(oversized) - 8, "do not", 7);
    oversized[sizeof(oversized)-1] = '\0';
    assert(!le_stop_intent_matches(oversized));
    puts("stop intent: imperative, negation, unrelated text and bounds: ok");
    return 0;
}
