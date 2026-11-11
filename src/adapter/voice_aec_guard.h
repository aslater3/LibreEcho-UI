#ifndef LIBREECHO_VOICE_AEC_GUARD_H
#define LIBREECHO_VOICE_AEC_GUARD_H

#include <stdint.h>

#include "voice_aec.h"
#include "voice_reference.h"

/* Live status file, rewritten in RAM only (never /data or flash). */
#define LE_VOICE_AEC_GUARD_STATUS_PATH "/run/libreecho/waked-aec.status"
#define LE_VOICE_AEC_GUARD_STATUS_INTERVAL_NS 10000000000ULL
#define LE_VOICE_AEC_GUARD_LOG_INTERVAL_NS 1000000000ULL
#define LE_VOICE_AEC_GUARD_STATUS_BYTES 512U

/* Keeps the echo canceller from using a filter that belongs to a previous
 * playback session or to a reference stream that lost packets. Detection
 * behaviour is otherwise untouched: it never gates or delays the mic. */
struct le_voice_aec_guard {
    char status_path[108];
    int previous_active;
    unsigned int seen_discontinuities;
    unsigned int aec_resets;
    unsigned int edge_resets;
    unsigned int discontinuity_resets;
    unsigned int suppressed_logs;
    uint64_t last_log_ns;
    uint64_t last_status_ns;
};

void le_voice_aec_guard_init(struct le_voice_aec_guard *guard,
                             const char *status_path);
/* Call once per mic frame, after the reference was drained and before AEC
 * runs. Returns 1 if the AEC was reset. */
int le_voice_aec_guard_update(struct le_voice_aec_guard *guard,
                              struct le_voice_aec *aec,
                              struct le_voice_reference *reference,
                              int reference_active,
                              uint64_t now_ns);
/* Rewrites the status file when due (or when force is set). */
void le_voice_aec_guard_status(struct le_voice_aec_guard *guard,
                               const struct le_voice_reference *reference,
                               int reference_active,
                               uint64_t aec_frames,
                               uint64_t now_ns,
                               int force);

#endif
