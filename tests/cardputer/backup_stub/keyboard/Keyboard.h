#pragma once
#include <vector>

/* Narrow keyboard surface used by board.h and input.cpp on the host. The
 * probe supplies key positions and the physical/shifted map. */

#define KEY_LEFT_CTRL 0x80
#define KEY_LEFT_SHIFT 0x81
#define KEY_LEFT_ALT 0x82
#define KEY_FN 0xFF
#define KEY_OPT 0x00
#define KEY_BACKSPACE 0x2A
#define KEY_TAB 0x2B
#define KEY_ENTER 0x28

struct Point2D_t {
    int x = 0;
    int y = 0;
};

struct KeyValue_t {
    char value_first = '\0';
    char value_second = '\0';
};

class Keyboard_Class {
public:
    void begin();
    void updateKeyList();
    bool available() const;
    const std::vector<Point2D_t>& keyList() const;
    KeyValue_t getKeyValue(const Point2D_t& position) const;
};
