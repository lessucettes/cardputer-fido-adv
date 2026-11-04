#include "board.h"

#include "M5Unified.h"

namespace cardputer {

void Board::set_volume(std::uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    volume_ = percent;
    M5.Speaker.setVolume(static_cast<std::uint8_t>(percent * 255u / 100u));
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
    }
}

}  // namespace cardputer
