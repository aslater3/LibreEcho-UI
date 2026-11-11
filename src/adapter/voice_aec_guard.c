#define _POSIX_C_SOURCE 200809L

#include "voice_aec_guard.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void le_voice_aec_guard_init(struct le_voice_aec_guard *guard,
                             const char *status_path)
{
    if (!guard)
        return;
    memset(guard, 0, sizeof(*guard));
    snprintf(guard->status_path, sizeof(guard->status_path), "%s",
             status_path ? status_path : "");
}

static void guard_log(struct le_voice_aec_guard *guard, uint64_t now_ns,
                      const char *why, unsigned int discontinuities)
{
    if (guard->last_log_ns != 0 &&
        now_ns - guard->last_log_ns < LE_VOICE_AEC_GUARD_LOG_INTERVAL_NS) {
        ++guard->suppressed_logs;
        return;
    }
    fprintf(stderr,
            "waked: %s t=%llus aec_resets=%u ref_discontinuities=%u "
            "(+%u more events since last line)\n",
            why, (unsigned long long)(now_ns / 1000000000ULL),
            guard->aec_resets, discontinuities, guard->suppressed_logs);
    guard->suppressed_logs = 0;
    guard->last_log_ns = now_ns;
}

int le_voice_aec_guard_update(struct le_voice_aec_guard *guard,
                              struct le_voice_aec *aec,
                              struct le_voice_reference *reference,
                              int reference_active,
                              uint64_t now_ns)
{
    const char *why = NULL;
    int cleared = 0;

    if (!guard || !aec || !reference)
        return 0;
    if (reference_active && !guard->previous_active) {
        /* Playback start after idle: the ring only holds stale samples
         * (nothing consumed them while idle), which would misalign the
         * reference against the echo.  The filter itself is kept: the
         * speaker-to-mic path is the same as in the previous session, so
         * its weights are a better start than a cold, unconverged one. */
        le_voice_reference_clear(reference);
        ++guard->edge_clears;
        cleared = 1;
    }
    if (reference->discontinuities != guard->seen_discontinuities) {
        /* The reference already cleared its ring when it saw the gap;
         * the filter still has the old alignment. */
        guard->seen_discontinuities = reference->discontinuities;
        ++guard->discontinuity_resets;
        why = "AEC reset: reference sequence gap";
    }
    guard->previous_active = reference_active;
    if (!why) {
        if (cleared)
            guard_log(guard, now_ns,
                      "reference ring cleared at playback start (AEC kept)",
                      reference->discontinuities);
        return 0;
    }
    le_voice_aec_reset(aec);
    ++guard->aec_resets;
    guard_log(guard, now_ns, why, reference->discontinuities);
    return 1;
}

void le_voice_aec_guard_status(struct le_voice_aec_guard *guard,
                               const struct le_voice_reference *reference,
                               int reference_active,
                               uint64_t aec_frames,
                               uint64_t now_ns,
                               int force)
{
    char text[LE_VOICE_AEC_GUARD_STATUS_BYTES];
    char tmp[sizeof(guard->status_path) + 8];
    FILE *file;
    int length;

    if (!guard || !reference || guard->status_path[0] == '\0')
        return;
    if (!force && guard->last_status_ns != 0 &&
        now_ns - guard->last_status_ns <
            LE_VOICE_AEC_GUARD_STATUS_INTERVAL_NS)
        return;
    guard->last_status_ns = now_ns ? now_ns : 1;
    length = snprintf(text, sizeof(text),
                      "ref_packets=%u\nref_malformed=%u\n"
                      "ref_underflows=%u\nref_discontinuities=%u\n"
                      "ref_overruns=%u\nref_active=%d\naec_frames=%llu\n"
                      "aec_resets=%u\nref_edge_clears=%u\n"
                      "aec_discontinuity_resets=%u\n",
                      reference->packets, reference->malformed_packets,
                      reference->underflows, reference->discontinuities,
                      reference->overruns, reference_active ? 1 : 0,
                      (unsigned long long)aec_frames, guard->aec_resets,
                      guard->edge_clears, guard->discontinuity_resets);
    if (length <= 0 || (size_t)length >= sizeof(text))
        return;
    snprintf(tmp, sizeof(tmp), "%s.tmp", guard->status_path);
    file = fopen(tmp, "w");
    if (!file)
        return;
    if (fwrite(text, 1, (size_t)length, file) != (size_t)length ||
        fclose(file) != 0) {
        unlink(tmp);
        return;
    }
    if (rename(tmp, guard->status_path) < 0)
        unlink(tmp);
}
