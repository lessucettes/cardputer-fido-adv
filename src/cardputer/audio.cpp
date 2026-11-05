#include "board.h"

#include "M5Unified.h"

namespace cardputer {

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
    switch (tone) {
        case Tone::Request:
            M5.Speaker.tone(880.0f, 60, 0, true);
            break;
        case Tone::Success:
            M5.Speaker.tone(1320.0f, 50, 0, true);
            break;
        case Tone::Failure:
            M5.Speaker.tone(330.0f, 120, 0, true);
            break;
        case Tone::KeyPress:
            M5.Speaker.tone(1046.5f, 9, -1, true);
            break;
        case Tone::Navigate:
            M5.Speaker.tone(1318.5f, 12, -1, true);
            break;
        case Tone::Confirm:
            M5.Speaker.tone(1568.0f, 18, -1, true);
            break;
        case Tone::Back:
            M5.Speaker.tone(659.3f, 16, -1, true);
            break;
    }
}

}  // namespace cardputer
