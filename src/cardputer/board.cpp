#include "board.h"

#include <cstdio>
#include <cstring>

#include "M5Unified.h"
#include "esp_timer.h"

namespace cardputer {

bool Board::begin() {
    auto config = M5.config();
    config.clear_display = true;
    config.output_power = false;
    config.internal_imu = false;
    config.internal_rtc = false;
    config.internal_mic = false;
    config.external_speaker_value = 0;
    config.external_display_value = 0;
    M5.begin(config);

    keyboard_.begin();
    keyboard_ok_ = M5.getBoard() == m5::board_t::board_M5CardputerADV &&
                   keyboard_.available();
    brightness_ = 60;
    volume_ = 0;
    M5.Display.setBrightness(static_cast<std::uint8_t>(brightness_ * 255u / 100u));
    M5.Speaker.setVolume(0);
    if (!keyboard_ok_) {
        show_message("Keyboard unavailable", "Check the ADV keyboard");
    }
    return keyboard_ok_;
}

std::uint64_t Board::now_ms() {
    return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
}

}  // namespace cardputer
