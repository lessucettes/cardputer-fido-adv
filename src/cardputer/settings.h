#pragma once
#include <array>
#include <cstdint>

namespace cardputer {

struct Settings {
    std::uint8_t volume = 30;
    std::uint8_t brightness = 60;
    std::uint8_t timeout_index = 2;
};

inline constexpr std::array<std::uint32_t, 5> screen_timeouts_ms = {
    15000, 30000, 60000, 120000, 0
};
inline constexpr std::array<const char*, 5> screen_timeout_labels = {
    "15 s", "30 s", "1 min", "2 min", "Never"
};

bool valid_settings(const Settings& settings);

using SettingsBytes = std::array<std::uint8_t, 4>;

SettingsBytes encode_settings(const Settings& settings);
bool decode_settings(const SettingsBytes& bytes, Settings& out);

/* Platform adapters. The native build returns false and keeps defaults. */
bool load_settings(Settings* out);
bool save_settings(const Settings& settings);

}  // namespace cardputer
