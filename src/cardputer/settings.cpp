#include "settings.h"

#if defined(ESP_PLATFORM)
#include "nvs.h"
#include "nvs_flash.h"
#endif

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

#if defined(ESP_PLATFORM)
namespace {
constexpr const char* kNvsNamespace = "cardfido_ui";
constexpr const char* kNvsKey = "settings_v1";

bool nvs_ready() {
    static int state = 0; /* 0 = unknown, 1 = ready, -1 = unavailable */
    if (state == 0) {
        const esp_err_t err = nvs_flash_init();
        state = err == ESP_OK ? 1 : -1;
    }
    return state == 1;
}
}  // namespace

bool load_settings(Settings* out) {
    if (out == nullptr || !nvs_ready()) {
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    SettingsBytes bytes{};
    std::size_t length = bytes.size();
    const esp_err_t err = nvs_get_blob(handle, kNvsKey, bytes.data(), &length);
    nvs_close(handle);
    if (err != ESP_OK || length != bytes.size()) {
        return false;
    }
    Settings decoded;
    if (!decode_settings(bytes, decoded)) {
        return false;
    }
    *out = decoded;
    return true;
}

bool save_settings(const Settings& settings) {
    if (!valid_settings(settings) || !nvs_ready()) {
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const SettingsBytes bytes = encode_settings(settings);
    esp_err_t err = nvs_set_blob(handle, kNvsKey, bytes.data(), bytes.size());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}
#else
bool load_settings(Settings* out) {
    if (out != nullptr) {
        *out = Settings{};
    }
    return false;
}

bool save_settings(const Settings& settings) {
    (void)settings;
    return false;
}
#endif

}  // namespace cardputer
