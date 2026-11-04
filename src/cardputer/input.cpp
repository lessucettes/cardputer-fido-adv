#include "board.h"

namespace cardputer {
namespace {

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

void Board::poll_input() {
    if (!keyboard_ok_) {
        keys_ = Keys{};
        return;
    }
    keyboard_.updateKeyList();
    keys_ = read_keys(keyboard_);
}

}  // namespace cardputer
