#include "board.h"
#include "M5Unified.h"

#include <cstdio>
#include <cstring>

TestM5 M5;
static int failures = 0;
static void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

int main(int argc, char**) {
    const bool buffered = argc == 1;
    canvas_available = buffered;
    cardputer::Board board;
    cardputer::Snapshot view;
    view.title = "A";
    view.status = "BC";
    board.render(view);
    check(buffered ? (M5.Display.pushes == 1 && M5.Display.clears == 0)
                   : (M5.Display.pushes == 0 && M5.Display.clears == 1),
          "navigation uses one push or a direct fallback if allocation fails");
    board.render(view);
    check(M5.Display.pushes + M5.Display.clears == 1,
          "unchanged frame does not transfer again");
    view.title = "AB";
    view.status = "C";
    board.render(view);
    check(M5.Display.text.find("AB\n") != std::string::npos,
          "moving text across field boundaries must update the frame");

    view.layout = cardputer::Layout::Prompt;
    view.title = "Sign in?";
    view.footer = "ENT:YES DEL:NO";
    view.row_count = 1;
    std::strcpy(view.rows[0].text, "example.com");
    board.render(view);
    const unsigned clears = M5.Display.clears;
    check(clears == (buffered ? 1u : 3u) && M5.Display.text.find("example.com") != std::string::npos,
          "presence prompt reaches the panel directly");
    view.status = ""; // A transient menu status expires behind the prompt.
    board.render(view);
    check(M5.Display.clears == clears,
          "invisible status expiry must not blank and redraw a prompt");
    std::strcpy(view.rows[0].text, "other.example");
    board.render(view);
    check(M5.Display.text.find("other.example") != std::string::npos,
          "changed prompt identity is rendered");
    view.screen_on = false;
    board.render(view);
    const unsigned sleeping_clears = M5.Display.clears;
    board.render(view);
    check(M5.Display.clears == sleeping_clears, "sleep only blanks the panel once");
    view.screen_on = true;
    board.render(view);
    check(M5.Display.text.find("other.example") != std::string::npos,
          "wake redraws even an otherwise unchanged screen");
    return failures != 0;
}
