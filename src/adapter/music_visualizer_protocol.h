/*
 * Frozen music-visualizer feature transport (v2).
 *
 * The producer (LibreEcho-Platform audio_visualizer) keeps the adapter
 * envelope v=1, cmd=visualizer, args.action=frame, args.owner and the legacy
 * 24-hex-character args.levels.  It adds args.feature_version=2 and a flat set
 * of numeric fields.  This unit parses and validates that object and tracks
 * producer-session ordering.  It is self-contained: it does not depend on the
 * daemon's own JSON scanner, so the offline simulator can share it.
 *
 * A frame without feature_version is a legacy v1 spectrum frame and remains
 * valid.  Malformed, stale, reordered or duplicate frames are rejected without
 * mutating accepted scene state; the caller decides ownership/expiry resets.
 */
#ifndef LE_MUSIC_VISUALIZER_PROTOCOL_H
#define LE_MUSIC_VISUALIZER_PROTOCOL_H

#include <stddef.h>

#define LE_MUSIC_FEATURE_VERSION 2

/* Event bitmask (frozen). */
enum le_music_event {
    LE_MUSIC_EVENT_KICK      = 1,
    LE_MUSIC_EVENT_SNARE     = 2,
    LE_MUSIC_EVENT_HIGH      = 4,
    LE_MUSIC_EVENT_FILL      = 8,
    LE_MUSIC_EVENT_BUILD     = 16,
    LE_MUSIC_EVENT_REENTRY   = 32,
    LE_MUSIC_EVENT_BREAKDOWN = 64,
    LE_MUSIC_EVENT_SECTION   = 128,
    LE_MUSIC_EVENT_DROP      = 256
};

struct le_music_features {
    unsigned int energy;
    unsigned int warmth;
    unsigned int brightness_axis;
    unsigned int density;
    unsigned int transientness;
    unsigned int groove;
    unsigned int build;
    unsigned int spaciousness;
    unsigned int loudness_fast;
    unsigned int loudness_slow;
    unsigned int onset_low;
    unsigned int onset_mid;
    unsigned int onset_high;
    unsigned int beat_strength;
    unsigned int beat_confidence;
    unsigned int novelty;
    unsigned int event_strength;
    unsigned int beat_phase;     /* 0..65535 */
    unsigned int bpm_x100;       /* 0..30000 */
    unsigned int events;         /* bitmask of enum le_music_event */
    unsigned int feature_version;
    unsigned int seq;            /* uint32 monotonic per session */
    unsigned int timestamp_ms;   /* monotonic milliseconds */
    unsigned int session;        /* nonzero uint32 */
};

/*
 * Parse a v2 feature object.  json/len describe the raw args object text.
 * Returns 1 when a valid v2 frame was parsed, 0 when the object is not a v2
 * frame (feature_version absent => legacy v1), -1 when the frame is malformed
 * or out of range.  Values are only written to *out on success.
 */
int le_music_parse_v2(const char *json, size_t len,
                      struct le_music_features *out);

/*
 * Producer-session ordering tracker.  Accept only strictly increasing seq and
 * non-decreasing timestamp within a session; a different nonzero session
 * starts a new stream.  Duplicate/reordered/stale frames are rejected without
 * touching any accepted state.
 */
struct le_music_stream {
    unsigned int session;
    unsigned int last_seq;
    unsigned long long last_timestamp_ms;
    int have_session;
};

void le_music_stream_reset(struct le_music_stream *stream);
/* 1 = accept (and update tracker), 0 = reject (tracker unchanged). */
int le_music_stream_accept(struct le_music_stream *stream,
                           const struct le_music_features *features);

#endif /* LE_MUSIC_VISUALIZER_PROTOCOL_H */
