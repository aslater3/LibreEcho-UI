/* Unit tests for the wake-word capture-health helpers and the 64-bit frame
   counter parser.

   The logs-page wake-word check must not read green just because waked is
   reachable and its model is loaded: a microphone-processing loop that has
   stalled keeps both of those true while processed_frames stops moving. VAD
   is not liveness either -- a healthy device in a quiet room reports
   vad_active=false -- and the counter passes INT_MAX after ~249 days of
   10 ms frames, so it is parsed 64-bit or not at all. */
#include "json.h"
#include "wake_health.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)

int main(void)
{
    struct le_wake_sample s;
    unsigned long long frames;

    /* First sample: nothing to compare yet, so health stays optimistic. */
    memset(&s, 0, sizeof(s));
    le_wake_sample_update(&s, 1000, 5000);
    CHECK(s.valid && !s.stalled && le_wake_capture_alive(&s));

    /* The same counter inside the stale window is not yet evidence. */
    le_wake_sample_update(&s, 1000, 5000 + LE_WAKE_CAPTURE_STALE_MS - 1);
    CHECK(!s.stalled && le_wake_capture_alive(&s));

    /* Frozen across a full window: the processing loop has stalled. */
    le_wake_sample_update(&s, 1000, 5000 + LE_WAKE_CAPTURE_STALE_MS);
    CHECK(s.valid && s.stalled && !le_wake_capture_alive(&s));

    /* Still frozen: the verdict holds. */
    le_wake_sample_update(&s, 1000, 5000 + 2 * LE_WAKE_CAPTURE_STALE_MS);
    CHECK(s.stalled && !le_wake_capture_alive(&s));

    /* Moving again: alive. A quiet-room frame advances the counter too. */
    le_wake_sample_update(&s, 1100, 5000 + 3 * LE_WAKE_CAPTURE_STALE_MS);
    CHECK(!s.stalled && le_wake_capture_alive(&s));

    /* A counter that reset because waked restarted has moved, not stalled. */
    le_wake_sample_update(&s, 5, 5000 + 4 * LE_WAKE_CAPTURE_STALE_MS);
    CHECK(!s.stalled && le_wake_capture_alive(&s));

    /* A dead clock (now_ms 0) leaves the retained sample alone. */
    memset(&s, 0, sizeof(s));
    le_wake_sample_update(&s, 42, 0);
    CHECK(!s.valid && le_wake_capture_alive(&s));
    le_wake_sample_update(&s, 42, 9000);
    le_wake_sample_update(&s, 42, 0);
    CHECK(s.valid && s.at_ms == 9000 && !s.stalled);

    /* A sample that was never taken cannot have proven a stall. */
    memset(&s, 0, sizeof(s));
    CHECK(le_wake_capture_alive(&s));

    /* json_get_u64 keeps the live counter beyond the int range ... */
    CHECK(json_get_u64("{\"processed_frames\":3000000000}", "processed_frames", &frames) == 1
          && frames == 3000000000ULL);
    CHECK(json_get_u64("{\"processed_frames\":4294967301}", "processed_frames", &frames) == 1
          && frames == 4294967301ULL);
    CHECK(json_get_u64("{\"processed_frames\":18446744073709551615}", "processed_frames", &frames) == 1
          && frames == 18446744073709551615ULL);
    /* ... and refuses a sign or an overflow rather than wrapping them. */
    CHECK(json_get_u64("{\"processed_frames\":-1}", "processed_frames", &frames) == -1);
    CHECK(json_get_u64("{\"processed_frames\":+1}", "processed_frames", &frames) == -1);
    CHECK(json_get_u64("{\"processed_frames\":18446744073709551616}", "processed_frames", &frames) == -1);
    CHECK(json_get_u64("{\"processed_frames\":\"12\"}", "processed_frames", &frames) == -1);
    CHECK(json_get_u64("{\"other\":1}", "processed_frames", &frames) == 0);

    /* A real waked status parses to the exact live reading. */
    CHECK(json_get_u64("{\"enabled\":true,\"wake_word\":\"Alexa\",\"model_status\":\"loaded\","
                       "\"vad_active\":false,\"processed_frames\":4294967301,\"model\":\"alexa_v0.1\"}",
                       "processed_frames", &frames) == 1 && frames == 4294967301ULL);
    puts("wake health: ok");
    return 0;
}
