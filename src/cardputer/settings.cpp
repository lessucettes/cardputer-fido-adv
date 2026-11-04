#include "settings.h"

namespace cardputer {

bool valid_settings(const Settings& settings) {
    return settings.volume <= 100 && settings.volume % 10 == 0 &&
           settings.brightness >= 10 && settings.brightness <= 100 &&
           settings.brightness % 10 == 0 &&
           settings.timeout_index < screen_timeouts_ms.size();
}

SettingsBytes encode_settings(const Settings& settings) {
    return {1, settings.volume, settings.brightness, settings.timeout_index};
}

bool decode_settings(const SettingsBytes& bytes, Settings& out) {
    const Settings candidate{bytes[1], bytes[2], bytes[3]};
    if (bytes[0] != 1 || !valid_settings(candidate)) {
        out = Settings{};
        return false;
    }
    out = candidate;
    return true;
}

}  // namespace cardputer
