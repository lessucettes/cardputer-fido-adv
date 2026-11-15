#include "board.h"
#include "M5Unified.h"

#include <cassert>
#include <algorithm>

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

    // Muting must discard an event waiting behind an active sound. Otherwise
    // leaving mute replays old feedback unrelated to the current key press.
    board.play(cardputer::Tone::KeyPress);
    board.play(cardputer::Tone::Failure);
    assert(M5.Speaker.accepted == 2);
    board.set_volume(0);
    M5.Speaker.playing = false;
    now = 3000;
    board.set_volume(30);
    assert(M5.Speaker.accepted == 2);
    board.play(cardputer::Tone::Navigate);
    assert(M5.Speaker.accepted == 3);

    // Rapid input must neither overwrite the DMA source nor start a second
    // voice. Only the newest pending event is kept, without a long backlog.
    for (int i = 0; i < 20; ++i) {
        board.play(cardputer::Tone::Confirm);
    }
    assert(M5.Speaker.accepted == 3);
    assert(std::equal(M5.Speaker.active_samples.begin(),
                      M5.Speaker.active_samples.end(), M5.Speaker.active_data));
    M5.Speaker.playing = false;
    now = 3001; // Mixer may report idle before the explicit busy deadline.
    board.service_audio();
    assert(M5.Speaker.accepted == 3);
    now = 4000;
    board.service_audio();
    assert(M5.Speaker.accepted == 4);
    M5.Speaker.playing = false;
    now = 5000;
    board.service_audio();
    assert(M5.Speaker.accepted == 4);
}
