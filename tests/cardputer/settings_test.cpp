#include "settings.h"
#include <cstdio>
#include <cstdlib>

using namespace cardputer;

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    const Settings defaults;
    require(valid_settings(defaults), "defaults must be valid");
    require(defaults.volume == 30, "first boot must have audible key feedback");
    const SettingsBytes encoded = encode_settings(defaults);
    require(encoded[0] == 1 && encoded[1] == 30 && encoded[2] == 60 &&
                encoded[3] == 2,
            "default byte sequence must be {1, 30, 60, 2}");

    Settings decoded;
    require(decode_settings(encoded, decoded), "defaults must decode");
    require(decoded.volume == 30 && decoded.brightness == 60 &&
                decoded.timeout_index == 2,
            "decoded defaults must match");

    require(!valid_settings(Settings{0, 0, 2}), "brightness 0 is invalid");
    require(!valid_settings(Settings{0, 5, 2}), "brightness 5 is invalid");
    require(!valid_settings(Settings{0, 105, 2}), "brightness 105 is invalid");
    require(!valid_settings(Settings{5, 60, 2}), "volume 5 is invalid");
    require(!valid_settings(Settings{110, 60, 2}), "volume 110 is invalid");
    require(!valid_settings(Settings{0, 60, 5}), "timeout index 5 is invalid");

    Settings invalid_out;
    const SettingsBytes wrong_version{2, 0, 60, 2};
    require(!decode_settings(wrong_version, invalid_out), "version 2 must fail");
    require(invalid_out.volume == 30 && invalid_out.brightness == 60 &&
                invalid_out.timeout_index == 2,
            "invalid version must fall back to safe defaults");

    const SettingsBytes bad_timeout{1, 0, 60, 5};
    require(!decode_settings(bad_timeout, invalid_out), "timeout index 5 must fail");

    const SettingsBytes bad_brightness{1, 0, 0, 2};
    require(!decode_settings(bad_brightness, invalid_out),
            "corrupt brightness must not blank the display");
    require(invalid_out.brightness == 60,
            "corrupt brightness must use the default, not zero");

    for (std::uint8_t volume = 0; volume <= 100; volume += 10) {
        for (std::uint8_t brightness = 10; brightness <= 100; brightness += 10) {
            for (std::uint8_t timeout = 0; timeout < 5; ++timeout) {
                const Settings candidate{volume, brightness, timeout};
                require(valid_settings(candidate), "generated settings must be valid");
                const SettingsBytes round = encode_settings(candidate);
                Settings result;
                require(decode_settings(round, result), "round trip must decode");
                require(result.volume == volume &&
                            result.brightness == brightness &&
                            result.timeout_index == timeout,
                        "round trip must preserve values");
            }
        }
    }

    std::puts("PASS: settings validation and byte contract");
    return 0;
}
