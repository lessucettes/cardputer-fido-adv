#pragma once

#include <cstdint>
#include <string>

inline constexpr std::uint16_t TFT_BLACK = 0;
namespace fonts { inline constexpr int Font0 = 0; }

// Record panel transfers separately from off-screen drawing.
struct TestDisplay {
    unsigned clears = 0;
    unsigned pushes = 0;
    std::string text;
    void fillScreen(std::uint16_t) { ++clears; text.clear(); }
    void fillRect(int, int, int, int, std::uint16_t) {}
    void drawRect(int, int, int, int, std::uint16_t) {}
    void setFont(const int*) {}
    void setTextWrap(bool) {}
    void setTextSize(int) {}
    void setTextColor(std::uint16_t, std::uint16_t) {}
    void setCursor(int, int) {}
    void print(const char* value) { text += value; text += '\n'; }
    void startWrite() {}
    void endWrite() {}
    void setBrightness(std::uint8_t) {}
};
struct TestM5 { TestDisplay Display; };
extern TestM5 M5;
inline bool canvas_available = true;
struct M5Canvas : TestDisplay {
    TestDisplay* panel;
    explicit M5Canvas(TestDisplay* display) : panel(display) {}
    void setPsram(bool) {}
    void setColorDepth(int) {}
    void* createSprite(int, int) { return canvas_available ? this : nullptr; }
    void pushSprite(int, int) { ++panel->pushes; panel->text = text; }
};
