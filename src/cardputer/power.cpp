#include "board.h"

#include "M5Unified.h"

namespace cardputer {

void Board::power_off() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.sleep();
    M5.Power.powerOff();
}

}  // namespace cardputer
