#pragma once
#include <cstddef>
#include <cstdint>

#include "model.h"

namespace cardputer {

enum class PresenceResult { None, Confirmed, Cancelled, TimedOut, Stale };

/*
 * Owner-context adapter between CTAP operations and the display/keyboard
 * model. One pending request at a time, no queued approvals, and every
 * decision is tied to the generation that produced it.
 */
class PresenceBridge {
public:
    void start(std::uint64_t now);
    std::uint32_t begin(RequestKind kind, const char* rp_id,
                        std::size_t rp_len, const char* account,
                        std::size_t account_len, std::uint64_t now,
                        std::uint64_t timeout_ms = 30000);
    void cancel(std::uint32_t generation, std::uint64_t now);
    void tick(std::uint64_t now);
    void key_snapshot(Keys keys, std::uint64_t now);
    PresenceResult take(std::uint32_t expected_generation);
    PresenceResult take_any(std::uint32_t* generation_out = nullptr);

    std::uint32_t active_generation() const {
        return model_.decision_pending() ? model_.decision_generation()
                                         : model_.active_generation();
    }
    bool pending() const { return model_.pending(); }
    const RequestInfo* active_request() const { return model_.active_request(); }
    Model& model() { return model_; }
    const Model& model() const { return model_; }

private:
    void clear_reset_state();

    Model model_;
    std::uint64_t last_now_ = 0;
    std::uint8_t reset_stage_ = 0;
    std::uint32_t reset_original_generation_ = 0;
    std::uint32_t reset_second_generation_ = 0;
};

}  // namespace cardputer
