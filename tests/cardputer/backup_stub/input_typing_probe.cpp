/*
 * Narrow host check for src/cardputer/input.cpp fresh-text collection.
 *
 * The production poll_input() body is compiled unchanged against a fake
 * keyboard that reports physical positions and the unshifted/shifted map.
 * Checks cover the physical control-code exclusions (Enter/Backspace/Tab are
 * printable-range values), shifted punctuation, held keys, modifiers and the
 * typed buffer bound. Password-loop consumption of a simultaneous final
 * character + Enter is checked by backup_runtime_probe.cpp.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "board.h"

using namespace cardputer;
static Board board;

namespace {

struct PositionMap {
    std::vector<Point2D_t> positions;
    std::map<std::pair<int, int>, KeyValue_t> values;
    bool available = true;
};

PositionMap keyboard_state;
int tone_calls = 0;
std::string typed_seen;
Keys keys_seen;

KeyValue_t value(char first, char second = '\0') {
    KeyValue_t item;
    item.value_first = first;
    item.value_second = second == '\0' ? first : second;
    return item;
}

void reset_keyboard() {
    keyboard_state.positions.clear();
    keyboard_state.values.clear();
}

void key(int x, int y, char first, char second = '\0') {
    keyboard_state.positions.push_back(Point2D_t{x, y});
    keyboard_state.values[{x, y}] = value(first, second);
}

void poll() {
    board.poll_input();
    typed_seen = board.typed();
    keys_seen = board.keys();
}

void release() {
    reset_keyboard();
    poll();
}

[[noreturn]] void fail(const std::string& message) {
    std::printf("FAIL input_typing: %s\n", message.c_str());
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) { fail(message); }
}

}  // namespace

/* Keyboard_Class shim. */
void Keyboard_Class::begin() {}
void Keyboard_Class::updateKeyList() {}
bool Keyboard_Class::available() const { return keyboard_state.available; }
const std::vector<Point2D_t>& Keyboard_Class::keyList() const {
    return keyboard_state.positions;
}
KeyValue_t Keyboard_Class::getKeyValue(const Point2D_t& position) const {
    const auto it = keyboard_state.values.find({position.x, position.y});
    if (it == keyboard_state.values.end()) { return KeyValue_t{}; }
    return it->second;
}

namespace cardputer {

bool Board::begin() {
    keyboard_ok_ = true;
    return true;
}

std::uint64_t Board::now_ms() { return 0; }
void Board::render(const Snapshot&) {}
void Board::show_message(const char*, const char*) {}
void Board::set_brightness(std::uint8_t) {}
void Board::set_volume(std::uint8_t) {}
void Board::play(Tone) { ++tone_calls; }
void Board::service_audio() {}
void Board::power_off() {}

}  // namespace cardputer

int main() {
    board.begin();

    /* Fresh printable key is collected once; holding it does not repeat. */
    reset_keyboard();
    key(0, 0, 'a', 'A');
    poll();
    require(typed_seen == "a", "fresh printable key must be collected");
    require(keys_seen.any, "press is reported");
    poll();
    require(typed_seen.empty(), "held key must not repeat text");
    release();

    /* Shifted punctuation resolves through the shifted map. */
    key(0, 1, KEY_LEFT_SHIFT);
    key(1, 1, '9', '(');
    poll();
    require(typed_seen == "(", "Shift+9 must type '(' not the physical '9'");
    require(board.fn_held() == false, "shift is not Fn");
    release();

    /* Enter physically carries 0x28, which is printable as '('. It must be
     * excluded by physical code, never inserted. */
    key(0, 2, KEY_ENTER, '(');
    poll();
    require(typed_seen.empty(), "Enter must not insert its printable code");
    require(keys_seen.enter, "Enter is still reported as a key");
    release();

    /* Tab physically carries 0x2b ('+'). It must not insert '+'. */
    key(1, 2, KEY_TAB, '+');
    poll();
    require(typed_seen.empty(), "Tab must not insert '+'");
    release();

    /* Backspace physically carries 0x2a ('*'). */
    key(2, 2, KEY_BACKSPACE, '*');
    poll();
    require(typed_seen.empty(), "Backspace must not insert '*'");
    require(keys_seen.del, "Backspace is still reported");
    release();

    /* Ctrl, Alt and Fn suppress text from simultaneous printable keys. */
    for (const char modifier : {static_cast<char>(KEY_LEFT_CTRL),
                                static_cast<char>(KEY_LEFT_ALT),
                                static_cast<char>(KEY_OPT),
                                static_cast<char>(KEY_FN)}) {
        key(0, 0, modifier);
        key(1, 0, 'x', 'X');
        poll();
        require(typed_seen.empty(), "modifier must suppress typed text");
        release();
    }

    /* Fn is exposed to the password screen for Fn+Del cancellation. */
    key(0, 0, KEY_FN);
    key(1, 0, 'x', 'X');
    poll();
    require(board.fn_held(), "Fn must be reported to the password screen");
    require(typed_seen.empty(), "Fn+printable stays suppressed");
    release();

    /* Space and '~' are the printable boundaries; DEL and control bytes are
     * not text even when they come from an unusual map. */
    key(0, 0, ' ', ' ');
    key(1, 0, '~', '~');
    poll();
    require(typed_seen == " ~", "printable boundaries must be collected");
    release();
    key(0, 0, static_cast<char>(0x7F));
    poll();
    require(typed_seen.empty(), "0x7F is not printable text");
    release();

    /* All 56 key positions can type once, with the buffer NUL terminated. */
    reset_keyboard();
    for (int index = 0; index < 56; ++index) {
        const int x = index % 14;
        const int y = index / 14;
        key(x, y, 'a');
    }
    poll();
    require(typed_seen.size() == 56, "all fresh printable keys are collected");
    require(typed_seen.find('\0') == std::string::npos,
            "typed buffer stays NUL terminated");
    release();

    /* A final character and Enter in the same poll are both available: the
     * input layer must not drop either. */
    reset_keyboard();
    key(0, 0, 'a', 'A');
    key(1, 0, KEY_ENTER);
    poll();
    require(typed_seen == "a", "character beside Enter is collected");
    require(keys_seen.enter, "Enter beside character is reported");
    release();

    std::printf("PASS input_typing\n");
    return 0;
}
