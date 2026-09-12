#ifndef LE_WAKE_HEALTH_H
#define LE_WAKE_HEALTH_H

/* Wake-word capture liveness.
 *
 * waked advances its processed_frames counter once per 10 ms captured frame,
 * so a live microphone loop moves it about a hundred frames a second. VAD is
 * not a liveness signal: on a healthy device in a quiet room vad_active is
 * false because the current frame holds no speech while capture runs on, so a
 * check that reads VAD as capture activity tells operators capture is dead
 * during silence. Capture health is therefore derived from successive samples
 * of the frame counter: a counter that has not moved across a full stale
 * window is a stalled microphone-processing loop -- the post-playback failure
 * a bare "the model is loaded" probe reads as green.
 *
 * The sample is retained across requests, because a single reading of a
 * counter says nothing about whether it is advancing.
 */
#define LE_WAKE_CAPTURE_STALE_MS 1000ULL

struct le_wake_sample {
    unsigned long long frames;   /* last accepted reading */
    unsigned long long at_ms;    /* monotonic milliseconds of that reading */
    int valid;                   /* a reading has been retained */
    int stalled;                 /* successive samples showed no advance */
};

/* Fold a fresh reading into the retained sample. A verdict is only reached
 * once a reading at least LE_WAKE_CAPTURE_STALE_MS newer than the retained
 * one is available; until then the last verdict stands, and the retained
 * reading deliberately stays put so the window keeps covering it. A counter
 * that moved -- including one that reset because waked restarted -- is
 * movement. A zero now_ms means the monotonic clock could not be read, so
 * there is no interval to judge; the retained sample is left alone. */
static inline void le_wake_sample_update(struct le_wake_sample *s,
                                         unsigned long long frames,
                                         unsigned long long now_ms)
{
    if (!s || !now_ms) return;
    if (!s->valid) { s->frames = frames; s->at_ms = now_ms; s->valid = 1; return; }
    if (now_ms - s->at_ms < LE_WAKE_CAPTURE_STALE_MS) return;
    s->stalled = frames == s->frames;
    s->frames = frames; s->at_ms = now_ms;
}

/* Capture reads as alive until successive samples prove it stalled: a single
 * sample, and any window too short to judge, can only be optimistic. */
static inline int le_wake_capture_alive(const struct le_wake_sample *s)
{
    return !(s && s->valid && s->stalled);
}

#endif
