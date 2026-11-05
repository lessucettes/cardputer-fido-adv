#include "board.h"

#include <cstring>

#include "M5Unified.h"
#include "view.h"

namespace cardputer {
namespace {

std::uint32_t hash_text(std::uint32_t hash, const char* text) {
    if (text == nullptr) {
        return hash;
    }
    while (*text != '\0') {
        hash ^= static_cast<std::uint8_t>(*text++);
        hash *= 16777619u;
    }
    return hash;
}

}  // namespace

void Board::render(const Snapshot& snapshot) {
    if (!snapshot.screen_on) {
        if (rendered_) {
            M5.Display.fillScreen(TFT_BLACK);
            rendered_ = false;
            last_hash_ = 0;
        }
        return;
    }

    std::uint32_t hash = 2166136261u;
    hash = hash_text(hash, snapshot.title);
    hash = hash_text(hash, snapshot.status);
    hash = hash_text(hash, snapshot.footer);
    hash = hash_text(hash, snapshot.value);
    hash ^= static_cast<std::uint32_t>(snapshot.layout);
    hash *= 16777619u;
    hash ^= static_cast<std::uint32_t>(snapshot.icon);
    hash *= 16777619u;
    hash ^= static_cast<std::uint32_t>(snapshot.meter);
    hash *= 16777619u;
    hash ^= static_cast<std::uint32_t>(snapshot.row_count);
    hash *= 16777619u;
    for (int i = 0; i < snapshot.row_count; ++i) {
        hash = hash_text(hash, snapshot.rows[i].text);
        hash ^= snapshot.rows[i].selected ? 1u : 0u;
        hash *= 16777619u;
        hash ^= static_cast<std::uint32_t>(snapshot.rows[i].icon);
        hash *= 16777619u;
    }
    if (rendered_ && hash == last_hash_) {
        return;
    }
    rendered_ = true;
    last_hash_ = hash;

    M5.Display.setFont(&fonts::Font0);
    M5.Display.startWrite();
    lcd::draw(M5.Display, snapshot);
    M5.Display.endWrite();
}

void Board::show_message(const char* title, const char* detail) {
    M5.Display.setFont(&fonts::Font0);
    lcd::message(M5.Display, title, detail);
    rendered_ = true;
    last_hash_ = 0;
}

void Board::set_brightness(std::uint8_t percent) {
    if (percent < 10) {
        percent = 10;
    }
    if (percent > 100) {
        percent = 100;
    }
    brightness_ = percent;
    M5.Display.setBrightness(static_cast<std::uint8_t>(percent * 255u / 100u));
}

}  // namespace cardputer
