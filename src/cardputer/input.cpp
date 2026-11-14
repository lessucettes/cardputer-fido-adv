#include "board.h"

namespace cardputer {
namespace {

constexpr unsigned kKeyboardColumns = 14;
constexpr unsigned kKeyboardRows = 4;
static_assert(kKeyboardColumns * kKeyboardRows <= 64,
              "keyboard positions must fit the press mask");

std::uint64_t position_bit(const Point2D_t& position) {
    if (position.x < 0 || position.y < 0 ||
        position.x >= static_cast<int>(kKeyboardColumns) ||
        position.y >= static_cast<int>(kKeyboardRows)) {
        return 0;
    }
    return std::uint64_t{1}
           << (static_cast<unsigned>(position.y) * kKeyboardColumns +
               static_cast<unsigned>(position.x));
}

Tone tone_for_key(std::uint8_t code) {
    switch (code) {
        case KEY_ENTER:
            return Tone::Confirm;
        case KEY_BACKSPACE:
            return Tone::Back;
        case ';':
        case '.':
        case ',':
        case '/':
            return Tone::Navigate;
        default:
            return Tone::KeyPress;
    }
}

Keys read_keys(Keyboard_Class& keyboard) {
    Keys keys;
    const auto& positions = keyboard.keyList();
    keys.any = !positions.empty();
    for (const auto& position : positions) {
        const std::uint8_t code = keyboard.getKeyValue(position).value_first;
        switch (code) {
            case KEY_ENTER:
                keys.enter = true;
                break;
            case KEY_BACKSPACE:
                keys.del = true;
                break;
            case ';':
                keys.up = true;
                break;
            case '.':
                keys.down = true;
                break;
            case ',':
                keys.left = true;
                break;
            case '/':
                keys.right = true;
                break;
            case KEY_FN:
            case KEY_LEFT_SHIFT:
            case KEY_LEFT_CTRL:
            case KEY_LEFT_ALT:
            case KEY_OPT:
                keys.modifier = true;
                break;
            default:
                break;
        }
    }
    return keys;
}

}  // namespace

void Board::queue_input_feedback(Tone tone) {
    if (pending_feedback_count_ >= kInputFeedbackCapacity) {
        for (std::uint8_t i = 1; i < kInputFeedbackCapacity; ++i) {
            pending_feedback_[i - 1] = pending_feedback_[i];
        }
        pending_feedback_[kInputFeedbackCapacity - 1] = tone;
        return;
    }
    pending_feedback_[pending_feedback_count_++] = tone;
}

void Board::flush_input_feedback() {
    const std::uint8_t count = pending_feedback_count_;
    pending_feedback_count_ = 0;
    for (std::uint8_t i = 0; i < count; ++i) {
        play(pending_feedback_[i]);
    }
    service_audio();
}

void Board::poll_input() {
    for (volatile char& ch : typed_) { ch = 0; }
    fn_held_ = false;
    // Bound feedback latency to one poll if the UI did not flush it right
    // after applying a settings change. Playing from here never sleeps.
    flush_input_feedback();
    service_audio();
    if (!keyboard_ok_) {
        keys_ = Keys{};
        pressed_positions_ = 0;
        return;
    }
    keyboard_.updateKeyList();

    const auto& positions = keyboard_.keyList();
    bool shift = false;
    bool suppress_text = false;
    for (const auto& position : positions) {
        const auto code = static_cast<std::uint8_t>(keyboard_.getKeyValue(position).value_first);
        shift = shift || code == KEY_LEFT_SHIFT;
        fn_held_ = fn_held_ || code == KEY_FN;
        suppress_text = suppress_text || code == KEY_LEFT_CTRL || code == KEY_LEFT_ALT ||
                        code == KEY_OPT || code == KEY_FN;
    }
    std::size_t typed_count = 0;
    std::uint64_t current_positions = 0;
    for (const auto& position : positions) {
        const std::uint64_t bit = position_bit(position);
        current_positions |= bit;
        if (bit != 0 && (pressed_positions_ & bit) == 0) {
            const auto code = static_cast<std::uint8_t>(
                keyboard_.getKeyValue(position).value_first);
            queue_input_feedback(tone_for_key(code));
            const auto value = keyboard_.getKeyValue(position);
            const auto character = static_cast<std::uint8_t>(shift ? value.value_second : value.value_first);
            if (!suppress_text && code != KEY_ENTER && code != KEY_BACKSPACE &&
                code != KEY_TAB && character >= 32 && character <= 126 &&
                typed_count + 1 < sizeof(typed_)) {
                typed_[typed_count++] = static_cast<char>(character);
            }
        }
    }
    pressed_positions_ = current_positions;
    typed_[typed_count] = '\0';

    keys_ = read_keys(keyboard_);
    service_audio();
}

}  // namespace cardputer
