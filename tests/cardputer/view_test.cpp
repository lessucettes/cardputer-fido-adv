#include "view.h"

#include <cstdlib>
#include <string>
#include <vector>

using namespace cardputer;

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct Box {
    int x, y, width, height;
};

struct Display {
    std::vector<Box> labels;
    std::string output;
    int x = 0, y = 0, scale = 2;
    bool cleared = false;

    void check(int left, int top, int width, int height) {
        require(left >= 0 && top >= 0 && width > 0 && height > 0 &&
                    left + width <= lcd::kWidth && top + height <= lcd::kHeight,
                "drawing must fit the physical 240x135 panel");
    }
    void fillScreen(std::uint16_t) { cleared = true; }
    void setTextWrap(bool wrap) { require(!wrap, "layout owns wrapping"); }
    void fillRect(int left, int top, int width, int height, std::uint16_t) {
        check(left, top, width, height);
    }
    void drawRect(int left, int top, int width, int height, std::uint16_t color) {
        fillRect(left, top, width, height, color);
    }
    void setTextSize(int size) {
        require(size >= 2, "no small or low-legibility 1x text");
        scale = size;
    }
    void setTextColor(std::uint16_t fg, std::uint16_t bg) {
        require((fg == lcd::kInk && bg == lcd::kPaper) ||
                    (fg == lcd::kPaper && bg == lcd::kInk),
                "all labels use the contrasting LCD palette");
    }
    void setCursor(int left, int top) { x = left; y = top; }
    void print(const char* text) {
        const int width = static_cast<int>(std::strlen(text)) * 6 * scale;
        if (width == 0) {
            return;
        }
        const Box next{x, y, width, 8 * scale};
        check(next.x, next.y, next.width, next.height);
        for (const auto& old : labels) {
            require(next.x >= old.x + old.width || old.x >= next.x + next.width ||
                        next.y >= old.y + old.height || old.y >= next.y + next.height,
                    "labels must not overlap each other or the footer");
        }
        labels.push_back(next);
        output += text;
        output += '\n';
    }
};

static Display render(const Model& model) {
    Display display;
    lcd::draw(display, model.view());
    require(display.cleared, "each changed view clears the old content");
    if (model.screen() != Screen::MainMenu || model.pending()) {
        require(display.output.find("USB") == std::string::npos,
                "local screens and prompts must not display the global USB status");
    }
    return display;
}

static void tap(Model& model, Keys keys) {
    model.key_snapshot(keys, 1);
    model.key_snapshot({}, 2);
}

int main() {
    const Keys enter{true, true};
    const Keys back{true, false, true};
    const Keys down{true, false, false, false, true};
    const Keys right{true, false, false, false, false, false, true};
    Model model;
    model.start(0);
    auto display = render(model);
    require(display.output.find("Saved Keys") != std::string::npos,
            "selected main menu card is readable");
    require(display.output.find("USB disconnected") != std::string::npos,
            "main menu shows a neutral disconnected status");
    model.set_usb_mounted(true);
    require(render(model).output.find("USB connected") != std::string::npos,
            "main menu reflects USB enumeration");
    tap(model, down);
    display = render(model);
    require(display.output.find("Saved Keys") == std::string::npos &&
                display.output.find("Settings") != std::string::npos,
            "navigation replaces the visible selected card");
    tap(model, enter);
    model.set_status("Save failed");
    require(render(model).output.find("Save failed") != std::string::npos,
            "local errors remain visible without a USB status");
    model.set_status("");
    for (int setting = 0; setting < 3; ++setting) {
        render(model);
        tap(model, enter);
        for (int value = 0; value < 12; ++value) {
            render(model);
            tap(model, right);
        }
        tap(model, back);
        tap(model, down);
    }
    tap(model, back);
    tap(model, down);
    render(model); // Power Off card, without executing its action.
    tap(model, down);
    tap(model, enter);
    render(model); // Empty key list.
    SavedKey keys[3] = {};
    std::strcpy(keys[0].rp_id, "a-long-relying-party.example.com");
    std::strcpy(keys[0].account, "a-long-but-readable-account-name");
    keys[1].protected_key = true;
    model.set_saved_keys(keys, 3);
    render(model);
    tap(model, enter);
    display = render(model);
    require(display.output.find("example.com") != std::string::npos &&
                display.output.find("account-name") != std::string::npos,
            "details wrap both RP and account instead of silently clipping them");
    tap(model, enter);
    render(model); // Local delete confirmation.

    for (const auto kind : {RequestKind::Generic, RequestKind::MakeCredential,
                           RequestKind::GetAssertion, RequestKind::Exclusion,
                           RequestKind::Reset, RequestKind::Selection}) {
        RequestInfo info;
        info.kind = kind;
        info.stage = 2;
        if (kind != RequestKind::Reset) {
            std::strcpy(info.rp_id, "example.com");
            std::strcpy(info.account, "alice");
        }
        model.begin_request(10, 100, info);
        display = render(model);
        require(display.output.find("ENT:YES DEL:NO") != std::string::npos,
                "every confirmation retains both actions at readable size");
        if (kind == RequestKind::Reset) {
            require(display.output.find("This cannot be") != std::string::npos &&
                        display.output.find("undone") != std::string::npos,
                    "destructive reset warning remains visible");
        } else {
            require(display.output.find("example.com") != std::string::npos &&
                        display.output.find("alice") != std::string::npos,
                    "presence prompts show RP and account");
        }
        model.cancel_request(11);
        (void)model.take_decision();
    }
    Display message;
    lcd::message(message, "Storage error", "The key store could not be read");
    Display unicode;
    lcd::text(unicode, "caf\xC3\xA9", 0, 0, 19);
    require(unicode.output == "caf?\n", "unsupported UTF-8 is one visible placeholder");
    model.set_screen_timeout(1);
    model.tick(1000);
    display = render(model);
    require(display.labels.empty(), "sleeping screen has no visible menu");
    std::puts("PASS: LCD menu, settings, identity, warnings and 240x135 geometry");
}
