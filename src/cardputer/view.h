#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "model.h"

namespace cardputer::lcd {

// Fixed 240x135 layout. Font0 has 6x8 cells; never use text smaller than 2x.
inline constexpr std::uint16_t kPaper = 0xEFB9;
inline constexpr std::uint16_t kInk = 0x1082;
inline constexpr int kWidth = 240;
inline constexpr int kHeight = 135;

// Original 16x16, one-bit pixel art. Integer scaling keeps the VMU silhouette.
inline constexpr std::uint16_t kIcons[][16] = {
    {0x0000,0x3C00,0x6600,0xC300,0xC300,0x6600,0x3F00,0x0380,
     0x01C0,0x00E0,0x0070,0x003B,0x001F,0x000E,0x0000,0x0000}, // Key
    {0x0000,0x03C0,0x0BD0,0x1FF8,0x3FFC,0x1E78,0x7C3E,0x781E,
     0x781E,0x7C3E,0x1E78,0x3FFC,0x1FF8,0x0BD0,0x03C0,0x0000}, // Settings
    {0x0180,0x0180,0x0180,0x1998,0x399C,0x6186,0x6186,0xC183,
     0xC183,0xC003,0x6006,0x6006,0x300C,0x1FF8,0x07E0,0x0000}, // Power
    {0x0000,0x0000,0x0208,0x0610,0x0E22,0x7E44,0x7E48,0x7E48,
     0x7E48,0x7E48,0x7E44,0x0E22,0x0610,0x0208,0x0000,0x0000}, // Volume
    {0x0180,0x0180,0x2184,0x1008,0x07E0,0x0C30,0x1818,0xD81B,
     0xD81B,0x1818,0x0C30,0x07E0,0x1008,0x2184,0x0180,0x0180}, // Sun
    {0x07E0,0x1FF8,0x381C,0x6186,0x6186,0xC183,0xC183,0xC1E3,
     0xC063,0xC003,0x6006,0x6006,0x300C,0x1C38,0x0FF0,0x03C0}, // Clock
    {0x0000,0x07E0,0x0C30,0x1818,0x1818,0x1818,0x7FFE,0x6006,
     0x6186,0x63C6,0x6186,0x6186,0x6006,0x7FFE,0x0000,0x0000}, // Lock
    {0x0000,0x07E0,0x07E0,0x7FFE,0x0000,0x300C,0x366C,0x366C,
     0x366C,0x366C,0x366C,0x366C,0x300C,0x1FF8,0x0000,0x0000}, // Trash
    {0x0000,0x0180,0x07E0,0x1FF8,0x781E,0x6006,0x6186,0x6186,
     0x6186,0x6186,0x300C,0x318C,0x1818,0x0C30,0x07E0,0x0180}  // Request
};

template <class Display>
void icon(Display& display, Icon image, int x, int y, int scale,
          std::uint16_t color = kInk) {
    const auto index = static_cast<unsigned>(image);
    const auto& pixels = kIcons[index < std::size(kIcons) ? index : 0];
    for (int row = 0; row < 16; ++row) {
        for (int col = 0; col < 16; ++col) {
            if ((pixels[row] & (0x8000u >> col)) != 0) {
                display.fillRect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

// ASCII Font0 is deliberate: render an unsupported UTF-8 scalar as one '?',
// not several unrelated glyphs. Never split a scalar or silently clip a suffix.
inline const char* next_glyph(const char* text, char* glyph) {
    const auto first = static_cast<unsigned char>(*text++);
    *glyph = first >= 32 && first < 127 ? static_cast<char>(first) : '?';
    if (first >= 128) {
        while ((*text & 0xC0) == 0x80) {
            ++text;
        }
    }
    return text;
}

template <class Display>
void text(Display& display, const char* input, int x, int y, int columns,
          int lines = 1, int scale = 2, std::uint16_t fg = kInk,
          std::uint16_t bg = kPaper) {
    if (input == nullptr || columns < 3 || columns > 39 || lines < 1) {
        return;
    }
    display.setTextSize(scale);
    display.setTextColor(fg, bg);
    const char* cursor = input;
    for (int line = 0; line < lines && *cursor != '\0'; ++line) {
        char buffer[40] = {};
        int length = 0;
        const char* start = cursor;
        const char* last_space = nullptr;
        int space_column = 0;
        while (*cursor != '\0' && length < columns) {
            if (*cursor == ' ') {
                last_space = cursor;
                space_column = length;
            }
            cursor = next_glyph(cursor, &buffer[length++]);
        }
        if (*cursor != '\0') {
            if (line == lines - 1) {
                std::memcpy(buffer + columns - 3, "...", 3);
            } else if (last_space != nullptr && last_space != start) {
                cursor = last_space + 1;
                buffer[space_column] = '\0';
            }
        }
        display.setCursor(x, y + line * (8 * scale + 2));
        display.print(buffer);
    }
}

template <class Display>
void frame(Display& display, const char* title, const char* footer, int columns = 19) {
    display.fillScreen(kPaper);
    display.setTextWrap(false);
    display.fillRect(0, 0, kWidth, 24, kInk);
    text(display, title, 6, 4, columns, 1, 2, kPaper, kInk);
    display.fillRect(0, 112, kWidth, 23, kInk);
    text(display, footer, 6, 116, 19, 1, 2, kPaper, kInk);
}

template <class Display>
void draw(Display& display, const Snapshot& snapshot) {
    if (!snapshot.screen_on) {
        display.fillScreen(0);
        return;
    }
    const bool carousel = snapshot.layout == Layout::Carousel;
    const bool prompt = snapshot.layout == Layout::Prompt;
    frame(display, prompt ? "FIDO CONFIRM" : snapshot.title, snapshot.footer,
          carousel ? 15 : 19);
    const int count = std::clamp(snapshot.row_count, 0, 4);
    switch (snapshot.layout) {
        case Layout::Carousel: {
            int selected = 0;
            for (int i = 0; i < count; ++i) {
                if (snapshot.rows[i].selected) {
                    selected = i;
                    break;
                }
            }
            if (count != 0) {
                icon(display, snapshot.rows[selected].icon, 12, 30, 3);
                // The selected card is the only menu item shown, with a bold
                // inverted label and a position counter, not color-only focus.
                display.fillRect(72, 28, 164, 58, kInk);
                text(display, snapshot.rows[selected].text, 76, 30, 13, 3, 2, kPaper, kInk);
                const char position[] = {static_cast<char>('1' + selected), '/',
                                         static_cast<char>('0' + count), '\0'};
                text(display, position, 198, 4, 3, 1, 2, kPaper, kInk);
                for (int step = 0; step < 3; ++step) {
                    display.fillRect(21 - step * 2, 81 + step * 2, 2 + step * 4, 2, kInk);
                    display.fillRect(41 - step * 2, 85 - step * 2, 2 + step * 4, 2, kInk);
                }
            }
            break;
        }
        case Layout::List:
            for (int i = 0; i < count && i < 3; ++i) {
                const int y = 28 + i * 20;
                const bool selected = snapshot.rows[i].selected;
                const auto fg = selected ? kPaper : kInk;
                const auto bg = selected ? kInk : kPaper;
                display.fillRect(4, y - 1, 232, 20, bg);
                icon(display, snapshot.rows[i].icon, 8, y, 1, fg);
                text(display, snapshot.rows[i].text, 28, y, 17, 1, 2, fg, bg);
            }
            break;
        case Layout::Details:
            // RP and account each get two full-width lines, rather than sharing
            // their space with decoration. The delete action lives in the footer.
            for (int i = 0; i < count && i < 2; ++i) {
                text(display, snapshot.rows[i].text, 6, 26 + i * 34, 19, 2);
            }
            break;
        case Layout::Prompt:
            icon(display, snapshot.icon, 8, 28, 2);
            text(display, snapshot.title, 48, 28, 15, 2);
            for (int i = 0; i < count && i < 2; ++i) {
                text(display, snapshot.rows[i].text, 6, 68 + i * 20, 19,
                     count == 1 ? 2 : 1);
            }
            break;
        case Layout::Editor:
            icon(display, snapshot.icon, 12, 30, 3);
            text(display, snapshot.value, 78, 38, 8, 1, 3);
            text(display, "<", 76, 68, 3);
            text(display, ">", 220, 68, 3);
            if (snapshot.meter >= 0) {
                display.drawRect(96, 70, 112, 12, kInk);
                const int value = std::clamp(snapshot.meter, 0, 100);
                if (value > 0) {
                    display.fillRect(99, 73, value * 106 / 100, 6, kInk);
                }
            }
            break;
    }
    if (!prompt) {
        text(display, snapshot.status, 6, 94, 19);
    }
}

template <class Display>
void message(Display& display, const char* title, const char* detail) {
    frame(display, title, "DEVICE HALTED");
    icon(display, Icon::Lock, 8, 32, 2);
    text(display, detail, 48, 30, 15, 4);
}

}  // namespace cardputer::lcd
