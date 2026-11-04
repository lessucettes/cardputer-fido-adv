#include "board.h"

#include <cstring>

#include "M5Unified.h"

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

void print_clipped(const char* text, std::size_t max_chars) {
    if (text == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < max_chars && text[i] != '\0'; ++i) {
        M5.Display.print(text[i]);
    }
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
    for (int i = 0; i < snapshot.row_count; ++i) {
        hash = hash_text(hash, snapshot.rows[i].text);
        hash ^= snapshot.rows[i].selected ? 1u : 0u;
        hash *= 16777619u;
    }
    if (rendered_ && hash == last_hash_) {
        return;
    }
    rendered_ = true;
    last_hash_ = hash;

    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextWrap(false);

    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(4, 4);
    print_clipped(snapshot.title, 19);

    M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 26);
    print_clipped(snapshot.status, 39);

    int y = 42;
    for (int i = 0; i < snapshot.row_count && i < 4; ++i) {
        M5.Display.setTextColor(snapshot.rows[i].selected ? TFT_YELLOW : TFT_WHITE,
                                TFT_BLACK);
        M5.Display.setTextSize(2);
        M5.Display.setCursor(4, y);
        M5.Display.print(snapshot.rows[i].selected ? "> " : "  ");
        print_clipped(snapshot.rows[i].text, 17);
        y += 22;
    }

    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, M5.Display.height() - 12);
    print_clipped(snapshot.footer, 39);
}

void Board::show_message(const char* title, const char* detail) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextWrap(false);
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(4, 12);
    print_clipped(title, 19);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 44);
    print_clipped(detail, 39);
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
