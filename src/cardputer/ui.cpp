#include "ui.h"

#include <cstring>

#include "M5Unified.h"
#include "board.h"
#include "credential_service.h"
extern "C" {
#include "file.h"
#include "flash.h"
}
#include "presence_bridge.h"
#include "settings.h"
#include "tusb.h"

extern "C" {
extern char* rp_id;
extern std::size_t rp_id_len;
extern char* user_name;

cf_credential_backend cardputer_credentials_backend(void) __attribute__((weak));
}

namespace cardputer {
namespace {

constexpr std::uint8_t kOpMakeCredential = 1;
constexpr std::uint8_t kOpGetAssertion = 2;

Board board;
PresenceBridge bridge;
cf_credential_service credentials;
Settings applied_settings;
Settings saved_settings;
bool usb_mounted = false;
int presence_kind_override = -1;
uint32_t last_tone_generation = 0;

void apply_settings(const Settings& settings) {
    board.set_brightness(settings.brightness);
    board.set_volume(settings.volume);
    bridge.model().set_screen_timeout(screen_timeouts_ms[settings.timeout_index]);
    applied_settings = settings;
}

void refresh_saved_keys() {
    size_t count = 0;
    int truncated = 0;
    const cf_credential_item* items =
        cf_credential_items(&credentials, &count, &truncated);
    SavedKey keys[Model::kMaxSavedKeys];
    int used = 0;
    for (size_t i = 0; i < count && used < Model::kMaxSavedKeys; ++i) {
        SavedKey& key = keys[used];
        std::memset(&key, 0, sizeof(key));
        std::memcpy(key.id, items[i].id, sizeof(key.id));
        key.id_len = items[i].id_len;
        std::memcpy(key.rp_id, items[i].rp_id, sizeof(key.rp_id) - 1);
        std::memcpy(key.account, items[i].account, sizeof(key.account) - 1);
        key.protected_key = items[i].protected_key != 0;
        ++used;
    }
    bridge.model().set_saved_keys(keys, used);
    if (truncated) {
        bridge.model().set_status("More keys than can be shown");
    }
}

void handle_action(const Action& action) {
    Model& model = bridge.model();
    switch (action.type) {
        case ActionType::OpenSavedKeys:
            if (cf_credential_request_list(&credentials) != CF_CRED_OK) {
                model.set_status("Request in progress");
            }
            break;
        case ActionType::SaveSettings:
            if (save_settings(action.settings)) {
                saved_settings = action.settings;
                apply_settings(saved_settings);
                model.set_settings(saved_settings);
                board.play(Tone::Success);
            } else {
                model.set_settings(saved_settings);
                apply_settings(saved_settings);
                model.set_status("Could not save settings");
                board.play(Tone::Failure);
            }
            break;
        case ActionType::DeleteKey: {
            const cf_cred_status status = cf_credential_request_delete(
                &credentials, action.key.id, action.key.id_len);
            if (status == CF_CRED_BUSY) {
                model.set_status("Request in progress");
            } else if (status != CF_CRED_OK) {
                model.set_status("Could not delete key");
            }
            break;
        }
        case ActionType::PowerOff:
            if (!flash_commit_sync(5000)) {
                model.set_status("Could not power off safely");
                board.play(Tone::Failure);
            } else {
                board.power_off();
            }
            break;
        default:
            break;
    }
}

void pump_credentials() {
    Model& model = bridge.model();
    const cf_cred_result result = cf_credential_service_poll(&credentials);
    if (result == CF_CRED_RESULT_LIST) {
        refresh_saved_keys();
    } else if (result == CF_CRED_RESULT_DELETED) {
        board.play(Tone::Success);
        refresh_saved_keys();
    } else if (result == CF_CRED_RESULT_NOT_FOUND) {
        model.set_status("Key not found");
        board.play(Tone::Failure);
    } else if (result == CF_CRED_RESULT_FORBIDDEN) {
        model.set_status("Protected key");
        board.play(Tone::Failure);
    } else if (result == CF_CRED_RESULT_ERROR) {
        model.set_status("Could not access saved keys");
        board.play(Tone::Failure);
    }
}

}  // namespace
}  // namespace cardputer

using namespace cardputer;

extern "C" void cardputer_ui_init(void) {
    board.begin();
    bridge.start(Board::now_ms());
    if (load_settings(&saved_settings)) {
        // Loaded values are already validated by the NVS adapter.
    } else {
        saved_settings = Settings{};
    }
    apply_settings(saved_settings);
    bridge.model().set_settings(saved_settings);

    cf_credential_backend backend = {};
    if (cardputer_credentials_backend != nullptr) {
        backend = cardputer_credentials_backend();
    }
    cf_credential_service_init(&credentials, backend);
}

extern "C" void cardputer_ui_pump(void) {
    const std::uint64_t now = Board::now_ms();
    board.poll_input();
    bridge.key_snapshot(board.keys(), now);
    bridge.tick(now);

    Model& model = bridge.model();
    const bool mounted = tud_mounted();
    if (mounted != usb_mounted) {
        if (!mounted) {
            bridge.cancel(bridge.active_generation(), now);
        }
        usb_mounted = mounted;
        model.set_usb_mounted(mounted);
    }

    Action action;
    if (model.take_action(&action)) {
        handle_action(action);
    }

    if (model.editing_settings()) {
        const Settings& draft = model.draft_settings();
        if (draft.brightness != board.brightness()) {
            board.set_brightness(draft.brightness);
        }
        if (draft.volume != board.volume()) {
            board.set_volume(draft.volume);
        }
    } else if (model.settings().volume != applied_settings.volume ||
               model.settings().brightness != applied_settings.brightness ||
               model.settings().timeout_index != applied_settings.timeout_index) {
        apply_settings(model.settings());
    }

    pump_credentials();

    if (bridge.pending() && bridge.active_generation() != last_tone_generation) {
        last_tone_generation = bridge.active_generation();
        board.play(Tone::Request);
    }
    board.render(model.view());
}

extern "C" std::uint32_t cardputer_presence_begin(std::uint32_t timeout_ms,
                                                   std::uint8_t operation) {
    RequestKind kind = RequestKind::Generic;
    if (presence_kind_override >= 0) {
        kind = static_cast<RequestKind>(presence_kind_override);
        presence_kind_override = -1;
    } else if (operation == kOpMakeCredential) {
        kind = RequestKind::MakeCredential;
    } else if (operation == kOpGetAssertion) {
        kind = RequestKind::GetAssertion;
    }
    const char* account = user_name != nullptr ? user_name : "";
    const std::size_t account_len = user_name != nullptr ? std::strlen(user_name) : 0;
    const char* rp = (kind == RequestKind::Reset) ? nullptr : rp_id;
    const std::size_t rp_len = rp != nullptr ? rp_id_len : 0;
    return bridge.begin(kind, rp, rp_len, account, account_len, Board::now_ms(),
                        timeout_ms != 0 ? timeout_ms : 30000);
}

extern "C" int cardputer_presence_take(std::uint32_t generation) {
    switch (bridge.take(generation)) {
        case PresenceResult::Confirmed:
            return 0;
        case PresenceResult::TimedOut:
            return 1;
        case PresenceResult::Cancelled:
        case PresenceResult::Stale:
            return 2;
        default:
            return -1;
    }
}

extern "C" void cardputer_presence_set_kind(int kind) {
    presence_kind_override = kind;
}

extern "C" void cardputer_storage_failure(int status) {
    (void)status;
    board.show_message("Storage error", "The key store could not be read");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
