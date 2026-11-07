#include "board.h"
#include "M5Unified.h"

#include <cassert>

TestM5 M5;
static std::uint64_t now = 0;

namespace cardputer {
std::uint64_t Board::now_ms() { return now; }

// No keyboard events in this test; retain the empty-flush audio servicing.
void Board::flush_input_feedback() { service_audio(); }
}  // namespace cardputer

int main() {
    cardputer::Board board;
    board.set_volume(30);
    board.play(cardputer::Tone::Request);
    assert(M5.Speaker.accepted == 0);

    // Several unsuccessful pumps must not consume the pending request.
    now = 1000;
    board.service_audio();
    assert(M5.Speaker.accepted == 0);
    M5.Speaker.available = true;
    board.service_audio();
    assert(M5.Speaker.accepted == 1);

    // Recovery plays the retained event once, not on every subsequent pump.
    M5.Speaker.playing = false;
    now = 2000;
    board.service_audio();
    assert(M5.Speaker.accepted == 1);
}
