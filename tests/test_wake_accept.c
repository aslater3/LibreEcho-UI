/*
 * Whether a scored frame should fire a wake event.
 *
 * The rule that matters here is the support requirement: a detection normally
 * needs two of the last three frames above the support threshold, which is what
 * stops a single noisy spike from waking the device. Measured on hardware, that
 * same requirement is why the wake word cannot be heard over music -- the score
 * reaches 0.807 for one frame and collapses to 0.212 on the next, so the second
 * frame never arrives and nothing fires, at any speaking volume.
 *
 * While the device is playing something, one frame is therefore enough. A false
 * wake during music costs a great deal less than being unable to stop it.
 */

#include "adapter/wake_accept.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const float threshold = 0.5508f;

    /* Quiet: one supporting frame is not enough, two are. */
    assert(!le_wake_accept(0.99f, threshold, 1, 1, 0));
    assert(le_wake_accept(0.99f, threshold, 2, 1, 0));
    assert(le_wake_accept(0.99f, threshold, 3, 1, 0));

    /* Playing: one is enough, which is the whole point of the change. */
    assert(le_wake_accept(0.99f, threshold, 1, 1, 1));
    assert(le_wake_accept(0.99f, threshold, 2, 1, 1));

    /* No support at all never fires, playing or not. */
    assert(!le_wake_accept(0.99f, threshold, 0, 1, 0));
    assert(!le_wake_accept(0.99f, threshold, 0, 1, 1));

    /*
     * The threshold still governs. Relaxing support must not turn the device
     * into something that wakes on the music itself.
     */
    assert(!le_wake_accept(0.20f, threshold, 1, 1, 1));
    assert(!le_wake_accept(0.20f, threshold, 3, 1, 1));
    assert(!le_wake_accept(0.55f, threshold, 2, 1, 0));   /* just under */
    assert(le_wake_accept(0.5508f, threshold, 2, 1, 0));  /* exactly at */

    /* Speech must still have been detected. */
    assert(!le_wake_accept(0.99f, threshold, 3, 0, 0));
    assert(!le_wake_accept(0.99f, threshold, 3, 0, 1));

    /*
     * The two traces actually recorded on the device. The first is the frame
     * that should have stopped the music and did not; the second is a healthy
     * detection in a quiet room, which must keep working unchanged.
     */
    assert(le_wake_accept(0.807615f, threshold, 1, 1, 1));
    assert(le_wake_accept(0.987937f, threshold, 2, 1, 0));

    /* And the frame either side of that music spike must stay silent. */
    assert(!le_wake_accept(0.299360f, threshold, 0, 1, 1));
    assert(!le_wake_accept(0.211981f, threshold, 1, 1, 1));

    puts("wake_accept: support relaxed only while playing, threshold intact");
    return 0;
}
