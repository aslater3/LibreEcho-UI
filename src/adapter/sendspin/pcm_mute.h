/*
 * Sendspin companion — source-only mute for interleaved S16 PCM.
 *
 * A server mute silences Sendspin audio only: the shared engine still mixes
 * alarms, timers and voice at full level. The companion applies it to the PCM
 * it hands the engine, with a short linear ramp so muting never clicks.
 *
 * Gain is a pure function of a frame's position in the stream (the sink's
 * acknowledged-frame count), and apply() writes a scaled copy. The SDK
 * re-presents frames the sink did not accept, so a retry must produce the
 * same bytes: scaling in place, or advancing a ramp per call, would apply the
 * gain twice. Not thread-safe; the owner serializes it (the listener mutex).
 *
 * No Sendspin SDK dependency; exercised by the dependency-free companion lane.
 */
#ifndef LIBREECHO_SENDSPIN_PCM_MUTE_H
#define LIBREECHO_SENDSPIN_PCM_MUTE_H

#include <cstddef>
#include <cstdint>

namespace libreecho::sendspin {

class PcmMute {
public:
    static constexpr uint32_t kUnity = 1u << 15;
    // 5 ms at 48 kHz from full level to silence.
    static constexpr uint32_t kRampFrames = 240;

    bool muted() const { return muted_; }

    // Mute or unmute, ramping from stream frame `at_frame` onwards.
    void set_muted(bool muted, uint64_t at_frame) {
        if (muted == muted_) return;
        edge_gain_ = gain_at(at_frame);
        edge_frame_ = at_frame;
        muted_ = muted;
    }

    // A new stream starts at frame 0 already at the current target level.
    void reset_stream() {
        edge_frame_ = 0;
        edge_gain_ = muted_ ? 0u : kUnity;
    }

    // Gain (Q15) of stream frame `frame`.
    uint32_t gain_at(uint64_t frame) const {
        const uint64_t since = frame > edge_frame_ ? frame - edge_frame_ : 0;
        const uint64_t moved = since * kStep;
        if (muted_) return moved >= edge_gain_ ? 0u : edge_gain_ - static_cast<uint32_t>(moved);
        return moved >= kUnity - edge_gain_ ? kUnity : edge_gain_ + static_cast<uint32_t>(moved);
    }

    // Copy `frames` frames starting at stream frame `first_frame` from `in` to
    // `out`, scaled. Returns false (out untouched) when every frame is at
    // unity, so the caller can pass `in` through unchanged.
    bool apply(const int16_t* in, int16_t* out, size_t frames, unsigned channels,
               uint64_t first_frame) const {
        if (gain_at(first_frame) == kUnity && gain_at(first_frame + frames) == kUnity &&
            !muted_)
            return false;
        for (size_t f = 0; f < frames; ++f) {
            const int32_t gain = static_cast<int32_t>(gain_at(first_frame + f));
            for (unsigned c = 0; c < channels; ++c) {
                const size_t i = f * channels + c;
                out[i] = static_cast<int16_t>((static_cast<int32_t>(in[i]) * gain) >> 15);
            }
        }
        return true;
    }

private:
    static constexpr uint32_t kStep = (kUnity + kRampFrames - 1) / kRampFrames;
    bool muted_ = false;
    uint64_t edge_frame_ = 0;
    uint32_t edge_gain_ = kUnity;
};

}  // namespace libreecho::sendspin

#endif
