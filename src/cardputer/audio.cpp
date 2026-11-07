#include "board.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "M5Unified.h"

namespace cardputer {
namespace {

constexpr std::uint32_t kToneSampleRate = 16000;
constexpr std::size_t kToneBufferSamples = 2400;
int16_t tone_buffer[kToneBufferSamples] = {};

struct ToneSpec {
    float frequency;
    std::uint32_t duration_ms;
    int16_t amplitude;
};

ToneSpec tone_spec(Tone tone) {
    switch (tone) {
        case Tone::Request:
            return {980.0f, 110, 18000};
        case Tone::Success:
            return {1320.0f, 70, 14000};
        case Tone::Failure:
            return {330.0f, 140, 14000};
        case Tone::KeyPress:
            return {1046.5f, 22, 9000};
        case Tone::Navigate:
            return {1318.5f, 24, 10000};
        case Tone::Confirm:
            return {1568.0f, 30, 12000};
        case Tone::Back:
            return {659.3f, 24, 10000};
    }
    return {1000.0f, 20, 9000};
}

}  // namespace

void Board::set_volume(std::uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    volume_ = percent;
    M5.Speaker.setVolume(static_cast<std::uint8_t>(percent * 255u / 100u));
    // A volume key press is detected before the model applies the new draft
    // volume. Replay queued feedback now so the first increase from mute is
    // audible, and a cancel back to mute still discards it silently.
    flush_input_feedback();
}

void Board::play(Tone tone) {
    if (volume_ == 0) {
        return;
    }
    queued_tone_ = tone;
    audio_queued_ = true;
    service_audio();
}

void Board::service_audio() {
    if (!audio_queued_ || volume_ == 0) {
        return;
    }
    // Never replace a tone that is already sounding: replacing it at a
    // non-zero crossing makes the tail sound chopped. Queue the newest event
    // and start it only when channel 0 is free.
    if (M5.Speaker.isPlaying(0) || Board::now_ms() < audio_busy_until_) {
        return;
    }
    // M5Unified 0.2.13 playRaw() returns true even if begin() fails.
    // Keep the pending event until the speaker can actually start.
    if (!M5.Speaker.begin()) {
        return;
    }
    const Tone tone = queued_tone_;
    const ToneSpec spec = tone_spec(tone);
    const std::size_t samples = std::min<std::size_t>(
        kToneBufferSamples, (spec.duration_ms * kToneSampleRate) / 1000u);
    const std::size_t fade = std::max<std::size_t>(1, samples / 8);
    for (std::size_t i = 0; i < samples; ++i) {
        float envelope = 1.0f;
        if (i < fade) {
            envelope = static_cast<float>(i) / static_cast<float>(fade);
        } else if (i + fade >= samples) {
            envelope = static_cast<float>(samples - i) / static_cast<float>(fade);
        }
        const float wave = std::sin(
            2.0f * 3.14159265358979323846f * spec.frequency * i /
            static_cast<float>(kToneSampleRate));
        tone_buffer[i] = static_cast<int16_t>(wave * envelope * spec.amplitude);
    }
    if (M5.Speaker.playRaw(tone_buffer, samples, kToneSampleRate, false, 1, 0,
                           true)) {
        audio_queued_ = false;
        audio_busy_until_ = Board::now_ms() + spec.duration_ms + 8;
    }
}

}  // namespace cardputer
