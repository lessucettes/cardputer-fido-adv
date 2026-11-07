#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

// Models the pinned M5Unified 0.2.13 startup contract, including playRaw's
// true return without accepting data when begin() fails.
struct TestSpeaker {
    bool available = false;
    bool playing = false;
    unsigned accepted = 0;

    bool begin() { return available; }
    void setVolume(std::uint8_t) {}
    std::size_t isPlaying(std::uint8_t channel) const {
        assert(channel == 0);
        return playing;
    }
    bool playRaw(const std::int16_t* data, std::size_t samples,
                 std::uint32_t rate, bool stereo, std::uint32_t repeat,
                 int channel, bool) {
        if (!begin()) {
            return true;
        }
        assert(data != nullptr && samples > 0 && samples <= 2400);
        assert(rate == 16000 && !stereo && repeat == 1 && channel == 0);
        assert(!playing);
        playing = true;
        ++accepted;
        return true;
    }
};

struct TestM5 {
    TestSpeaker Speaker;
};
extern TestM5 M5;
