#include "presence_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace cardputer;

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static Keys key_enter() {
    Keys keys;
    keys.any = true;
    keys.enter = true;
    return keys;
}

static Keys key_del() {
    Keys keys;
    keys.any = true;
    keys.del = true;
    return keys;
}

static void tap(PresenceBridge& bridge, Keys keys, std::uint64_t now) {
    bridge.key_snapshot(keys, now);
    bridge.key_snapshot(Keys{}, now + 1);
}

static void test_fresh_presence() {
    PresenceBridge bridge;
    bridge.start(0);
    bridge.key_snapshot(key_enter(), 90);
    const std::uint32_t generation =
        bridge.begin(RequestKind::MakeCredential, "example.com", 11, "alice", 5,
                     100, 1000);
    require(bridge.pending(), "request is pending");
    require(std::strcmp(bridge.model().view().title, "Create passkey?") == 0,
            "create title");

    bridge.key_snapshot(key_enter(), 101);
    require(bridge.take(generation) == PresenceResult::None,
            "held Enter must not approve");
    bridge.key_snapshot(Keys{}, 102);
    bridge.key_snapshot(key_enter(), 103);
    require(bridge.take(generation) == PresenceResult::Confirmed,
            "fresh Enter confirms");
    require(bridge.take(generation) == PresenceResult::None,
            "decision is not queued");

    bridge.key_snapshot(key_enter(), 190);
    const std::uint32_t next =
        bridge.begin(RequestKind::GetAssertion, "example.com", 11, "alice", 5,
                     200, 1000);
    bridge.key_snapshot(key_enter(), 201);
    require(bridge.take(next) == PresenceResult::None,
            "held Enter does not approve the next request");
}

static void test_stale_and_cancel(void) {
    PresenceBridge bridge;
    bridge.start(0);
    const std::uint32_t first =
        bridge.begin(RequestKind::GetAssertion, "a.example", 9, nullptr, 0, 100,
                     1000);
    bridge.cancel(first, 101);
    require(bridge.take(first) == PresenceResult::Cancelled,
            "host cancellation is reported");

    const std::uint32_t second =
        bridge.begin(RequestKind::GetAssertion, "b.example", 9, nullptr, 0, 200,
                     1000);
    bridge.cancel(second, 201);
    require(bridge.take(first) == PresenceResult::Stale,
            "old generation cannot consume a new cancellation");
    require(bridge.take(second) == PresenceResult::Cancelled,
            "new generation receives its cancellation");

    const std::uint32_t third =
        bridge.begin(RequestKind::GetAssertion, "c.example", 9, nullptr, 0, 300,
                     1000);
    bridge.key_snapshot({true, true, true}, 301);
    require(bridge.take(third) == PresenceResult::Cancelled,
            "Del wins over a simultaneous Enter");
}

static void test_timeout(void) {
    PresenceBridge bridge;
    bridge.start(0);
    const std::uint32_t generation =
        bridge.begin(RequestKind::GetAssertion, "example.com", 11, nullptr, 0,
                     100, 50);
    bridge.tick(150);
    require(bridge.take(generation) == PresenceResult::TimedOut,
            "expired request times out");
    require(!bridge.pending(), "timed out request is no longer pending");
}

static void test_reset_requires_two_presses() {
    PresenceBridge bridge;
    bridge.start(0);
    const std::uint32_t generation =
        bridge.begin(RequestKind::Reset, "example.com", 11, nullptr, 0, 100,
                     1000);

    tap(bridge, key_enter(), 200);
    require(bridge.take(generation) == PresenceResult::None,
            "first Enter only arms the deletion");
    require(bridge.pending(), "reset remains pending after arming");
    require(std::strcmp(bridge.model().view().title, "Confirm deletion") == 0,
            "second stage title");

    tap(bridge, key_enter(), 300);
    require(bridge.take(generation) == PresenceResult::Confirmed,
            "second fresh Enter confirms deletion");
    require(!bridge.pending(), "reset completed");

    const std::uint32_t cancelled =
        bridge.begin(RequestKind::Reset, "example.com", 11, nullptr, 0, 400,
                     1000);
    tap(bridge, key_enter(), 500);
    require(bridge.take(cancelled) == PresenceResult::None, "first stage armed");
    tap(bridge, key_del(), 600);
    require(bridge.take(cancelled) == PresenceResult::Cancelled,
            "Del at the second stage leaves credentials intact");
}

static void test_bounded_context(void) {
    PresenceBridge bridge;
    bridge.start(0);
    char rp[300];
    std::memset(rp, 'a', sizeof(rp));
    rp[sizeof(rp) - 1] = '\0';
    bridge.begin(RequestKind::MakeCredential, rp, sizeof(rp) - 1, "evil\nname",
                 9, 100, 1000);
    const RequestInfo* info = bridge.active_request();
    require(info != nullptr, "active request");
    require(std::strlen(info->rp_id) < sizeof(info->rp_id),
            "RP is bounded");
    require(std::strchr(info->account, '\n') == nullptr,
            "account cannot inject a newline");
    require(std::strlen(info->account) <= 9, "account stays bounded");
}

int main() {
    test_fresh_presence();
    test_stale_and_cancel();
    test_timeout();
    test_reset_requires_two_presses();
    test_bounded_context();
    std::puts("PASS: generation-gated presence and two-stage reset");
    return 0;
}
