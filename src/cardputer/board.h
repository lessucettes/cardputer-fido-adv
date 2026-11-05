#pragma once
#include <cstdint>

#include "keyboard/Keyboard.h"
#include "model.h"

namespace cardputer {

enum class Tone {
    Request,
    Success,
    Failure,
    KeyPress,
    Navigate,
    Confirm,
    Back,
};

class Board {
public:
    bool begin();
    bool keyboard_available() const { return keyboard_ok_; }
    void poll_input();
    Keys keys() const { return keys_; }
    static std::uint64_t now_ms();
    void render(const Snapshot& snapshot);
    void show_message(const char* title, const char* detail);
    void set_brightness(std::uint8_t percent);
    void set_volume(std::uint8_t percent);
    std::uint8_t brightness() const { return brightness_; }
    std::uint8_t volume() const { return volume_; }
    void play(Tone tone);
    void flush_input_feedback();
    void power_off();

private:
    static constexpr std::uint8_t kInputFeedbackCapacity = 8;

    void queue_input_feedback(Tone tone);

    Keyboard_Class keyboard_;
    Keys keys_{};
    bool keyboard_ok_ = false;
    bool rendered_ = false;
    std::uint32_t last_hash_ = 0;
    std::uint8_t brightness_ = 60;
    std::uint8_t volume_ = 0;
    std::uint64_t pressed_positions_ = 0;
    Tone pending_feedback_[kInputFeedbackCapacity] = {};
    std::uint8_t pending_feedback_count_ = 0;
};

}  // namespace cardputer
