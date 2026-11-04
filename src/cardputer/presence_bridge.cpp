#include "presence_bridge.h"

#include <cstring>

#include "text.h"

namespace cardputer {

void PresenceBridge::start(std::uint64_t now) {
    model_.start(now);
    last_now_ = now;
    clear_reset_state();
}

void PresenceBridge::clear_reset_state() {
    reset_stage_ = 0;
    reset_original_generation_ = 0;
    reset_second_generation_ = 0;
}

std::uint32_t PresenceBridge::begin(RequestKind kind, const char* rp_id,
                                    std::size_t rp_len, const char* account,
                                    std::size_t account_len, std::uint64_t now,
                                    std::uint64_t timeout_ms) {
    RequestInfo info;
    info.kind = kind;
    if (rp_id != nullptr && rp_len > 0) {
        sanitize_text(rp_id, rp_len, info.rp_id, sizeof(info.rp_id) - 1);
    }
    if (account != nullptr && account_len > 0) {
        sanitize_text(account, account_len, info.account, sizeof(info.account) - 1);
    }
    clear_reset_state();
    if (kind == RequestKind::Reset) {
        reset_stage_ = 1;
        info.stage = 1;
    }
    last_now_ = now;
    const std::uint32_t generation = model_.begin_request(now, timeout_ms, info);
    if (kind == RequestKind::Reset) {
        reset_original_generation_ = generation;
    }
    return generation;
}

void PresenceBridge::cancel(std::uint32_t generation, std::uint64_t now) {
    last_now_ = now;
    if (model_.pending() && model_.active_generation() == generation) {
        model_.cancel_request(now);
    }
}

void PresenceBridge::tick(std::uint64_t now) {
    last_now_ = now;
    model_.tick(now);
}

void PresenceBridge::key_snapshot(Keys keys, std::uint64_t now) {
    last_now_ = now;
    model_.key_snapshot(keys, now);
}

PresenceResult PresenceBridge::take(std::uint32_t expected_generation) {
    if (expected_generation == 0) {
        return PresenceResult::Stale;
    }
    const RequestInfo request = model_.request_info();
    if (!model_.decision_pending()) {
        return PresenceResult::None;
    }
    const std::uint32_t decision_generation = model_.decision_generation();
    const bool matches =
        decision_generation == expected_generation ||
        (reset_stage_ == 2 && expected_generation == reset_original_generation_ &&
         decision_generation == reset_second_generation_);
    if (!matches) {
        return PresenceResult::Stale;
    }
    const Decision decision = model_.take_decision();
    if (decision == Decision::None) {
        return PresenceResult::None;
    }
    if (decision == Decision::Cancel) {
        clear_reset_state();
        return PresenceResult::Cancelled;
    }
    if (decision == Decision::TimedOut) {
        clear_reset_state();
        return PresenceResult::TimedOut;
    }

    const bool reset = request.kind == RequestKind::Reset;
    if (reset && reset_stage_ == 1) {
        RequestInfo second = request;
        second.stage = 2;
        reset_stage_ = 2;
        reset_second_generation_ =
            model_.begin_request(last_now_, 30000, second);
        return PresenceResult::None;
    }
    clear_reset_state();
    return PresenceResult::Confirmed;
}

PresenceResult PresenceBridge::take_any(std::uint32_t* generation_out) {
    const RequestInfo request = model_.request_info();
    const Decision decision = model_.take_decision();
    if (decision == Decision::None) {
        return PresenceResult::None;
    }
    const std::uint32_t generation = model_.decision_generation();
    if (generation_out != nullptr) {
        *generation_out = generation;
    }
    if (decision == Decision::Cancel) {
        clear_reset_state();
        return PresenceResult::Cancelled;
    }
    if (decision == Decision::TimedOut) {
        clear_reset_state();
        return PresenceResult::TimedOut;
    }
    const bool reset = request.kind == RequestKind::Reset;
    if (reset && reset_stage_ == 1) {
        RequestInfo second = request;
        second.stage = 2;
        reset_stage_ = 2;
        reset_second_generation_ =
            model_.begin_request(last_now_, 30000, second);
        return PresenceResult::None;
    }
    clear_reset_state();
    return PresenceResult::Confirmed;
}

}  // namespace cardputer
