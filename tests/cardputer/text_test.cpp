#include "text.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace cardputer;

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    char out[64];
    bool truncated = false;

    const char* hostile = "bank.example\nWaiting for a request";
    sanitize_text(hostile, std::strlen(hostile), out, sizeof(out), &truncated);
    require(std::strchr(out, '\n') == nullptr, "newline must not reach the display");
    require(std::strstr(out, "bank.example") != nullptr, "prefix preserved");
    require(!truncated, "short text is not truncated");

    const char* formatting = "100%% %s %n";
    sanitize_text(formatting, std::strlen(formatting), out, sizeof(out));
    require(std::strcmp(out, "100%% %s %n") == 0,
            "format specifiers must be copied literally");

    char long_text[300];
    std::memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    sanitize_text(long_text, std::strlen(long_text), out, 16, &truncated);
    require(truncated, "long text reports truncation");
    require(std::strlen(out) <= 15, "truncated text fits the buffer");
    require(std::strlen(out) >= 3 && std::strcmp(out + std::strlen(out) - 3, "...") == 0,
            "truncation is visible");

    const char invalid[] = {'a', static_cast<char>(0xC3), '(', 'b', '\0'};
    sanitize_text(invalid, 4, out, sizeof(out));
    require(std::strstr(out, "a") != nullptr && std::strstr(out, "b") != nullptr,
            "invalid bytes do not swallow neighbours");
    require(std::strstr(out, "\xEF\xBF\xBD") != nullptr,
            "invalid bytes become U+FFFD");

    const char bidi[] = "evil\xE2\x80\xAE" "example.com";
    sanitize_text(bidi, sizeof(bidi) - 1, out, sizeof(out));
    require(std::strcmp(out, "evilexample.com") == 0,
            "bidi override removed without breaking the label");

    const char utf8[] = "caf\xC3\xA9!!!";
    sanitize_text(utf8, sizeof(utf8) - 1, out, 7, &truncated);
    require(std::strcmp(out, "caf...") == 0,
            "multi-byte sequence is not split");
    require(valid_utf8(reinterpret_cast<const std::uint8_t*>(out), std::strlen(out)),
            "truncated output stays valid UTF-8");

    require(valid_utf8(reinterpret_cast<const std::uint8_t*>("plain"), 5),
            "ASCII is valid UTF-8");
    require(!valid_utf8(reinterpret_cast<const std::uint8_t*>(invalid), 4),
            "malformed input is rejected by the validator");

    shorten_middle("very-long-subdomain.evil.example.com", out, 20);
    require(std::strlen(out) <= 19, "middle shortening fits");
    require(std::strcmp(out + std::strlen(out) - 4, ".com") == 0,
            "middle shortening preserves the suffix");
    require(std::strstr(out, "...") != nullptr, "middle shortening is visible");

    std::puts("PASS: bounded untrusted text rendering");
    return 0;
}
