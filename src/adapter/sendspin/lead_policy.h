// Startup lead the player asks the server for, by output state.
//
// Opening the speaker PCM from idle takes ~1.4 s on the Echo Dot, so a cold
// stream start needs a long lead. The audio engine keeps its output running
// for a fixed hold after the last live Sendspin generation (Platform
// SENDPIN_OUTPUT_HOLD_US), so while streaming and for that hold (less a safety
// margin) after a stream ends, a seek or resume reuses the running output and
// only the pipeline lead is needed. update() returns the runtime lead override
// for PlayerRole::set_required_lead_time_ms(): `warm_ms` while warm, 0 (the
// configured cold lead) otherwise.
#ifndef LIBREECHO_SENDSPIN_LEAD_POLICY_H
#define LIBREECHO_SENDSPIN_LEAD_POLICY_H

#include <cstdint>

namespace libreecho::sendspin {

class LeadPolicy {
public:
    LeadPolicy(uint16_t warm_ms, int64_t hold_us, int64_t margin_us)
        : warm_ms_(warm_ms), warm_span_us_(hold_us > margin_us ? hold_us - margin_us : 0) {}

    uint16_t update(bool streaming, int64_t now_us) {
        if (streaming) {
            warm_until_us_ = now_us + warm_span_us_;
            return warm_ms_;
        }
        return now_us < warm_until_us_ ? warm_ms_ : 0;
    }

private:
    uint16_t warm_ms_;
    int64_t warm_span_us_;
    int64_t warm_until_us_ = 0;
};

}  // namespace libreecho::sendspin

#endif
