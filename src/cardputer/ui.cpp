#include "ui.h"

#include <cstring>

#include "M5Unified.h"
#include "board.h"
#include "credential_service.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif
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
extern bool is_busy(void);

cf_credential_backend cardputer_credentials_backend(void) __attribute__((weak));
}

namespace cardputer {
namespace {

constexpr std::uint8_t kOpMakeCredential = 1;
constexpr std::uint8_t kOpGetAssertion = 2;

static_assert(static_cast<int>(RequestKind::Exclusion) ==
                  CF_PRESENCE_KIND_EXCLUSION,
              "exclusion prompt kind must match the C protocol core");
static_assert(static_cast<int>(RequestKind::Reset) == CF_PRESENCE_KIND_RESET,
              "reset prompt kind must match the C protocol core");
static_assert(static_cast<int>(RequestKind::Selection) ==
                  CF_PRESENCE_KIND_SELECTION,
              "selection prompt kind must match the C protocol core");

Board board;
PresenceBridge bridge;
cf_credential_service credentials;
Settings applied_settings;
Settings saved_settings;
bool usb_mounted = false;
int presence_kind_override = -1;
volatile bool power_off_started = false;
volatile bool power_off_failed = false;
volatile uint32_t usb_sof_ms = 0;
volatile bool usb_sof_seen = false;
volatile bool usb_suspended = false;

#ifdef ESP_PLATFORM
void power_off_task(void*) {
    if (!flash_commit_sync(5000)) {
        power_off_failed = true;
        vTaskDelete(NULL);
        return;
    }
    board.power_off();
    vTaskDelete(NULL);
}
#endif

#ifdef ESP_PLATFORM
SemaphoreHandle_t ui_mutex = nullptr;

void ui_lock() {
    if (ui_mutex != nullptr) {
        xSemaphoreTake(ui_mutex, portMAX_DELAY);
    }
}

void ui_unlock() {
    if (ui_mutex != nullptr) {
        xSemaphoreGive(ui_mutex);
    }
}
#else
void ui_lock() {}
void ui_unlock() {}
#endif

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
        bridge.model().set_status("Key list limited", Board::now_ms());
    }
}

void handle_action(const Action& action) {
    Model& model = bridge.model();
    switch (action.type) {
        case ActionType::OpenSavedKeys:
            if (cf_credential_request_list(&credentials) != CF_CRED_OK) {
                model.set_status("Request in progress", Board::now_ms());
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
                model.set_status("Save failed", Board::now_ms());
                board.play(Tone::Failure);
            }
            break;
        case ActionType::DeleteKey: {
            const cf_cred_status status = cf_credential_request_delete(
                &credentials, action.key.id, action.key.id_len);
            if (status == CF_CRED_BUSY) {
                model.set_status("Request in progress", Board::now_ms());
            } else if (status != CF_CRED_OK) {
                model.set_status("Delete failed", Board::now_ms());
            }
            break;
        }
        case ActionType::PowerOff:
            board.flush_input_feedback();
#ifdef ESP_PLATFORM
            if (!power_off_started) {
                power_off_started = true;
                model.set_status("Powering off...", Board::now_ms());
                if (xTaskCreate(power_off_task, "fido_poweroff", 4096, NULL, 1,
                                NULL) != pdPASS) {
                    power_off_started = false;
                    model.set_status("Power off failed", Board::now_ms());
                    board.play(Tone::Failure);
                }
            }
#else
            if (!flash_commit_sync(5000)) {
                model.set_status("Power off failed", Board::now_ms());
                board.play(Tone::Failure);
            } else {
                board.power_off();
            }
#endif
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
        model.set_status("Key not found", Board::now_ms());
        board.play(Tone::Failure);
    } else if (result == CF_CRED_RESULT_FORBIDDEN) {
        model.set_status("Protected key", Board::now_ms());
        board.play(Tone::Failure);
    } else if (result == CF_CRED_RESULT_ERROR) {
        model.set_status("Key list failed", Board::now_ms());
        board.play(Tone::Failure);
    }
}

}  // namespace
}  // namespace cardputer

using namespace cardputer;

extern "C" void cardputer_ui_init(void) {
#ifdef ESP_PLATFORM
    if (ui_mutex == nullptr) {
        ui_mutex = xSemaphoreCreateMutex();
    }
#endif
    ui_lock();
    if (!board.begin()) {
        // Keep the fatal keyboard message visible; do not start a menu or
        // USB authenticator that cannot receive presence confirmations.
        ui_unlock();
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
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
    ui_unlock();
}

extern "C" void cardputer_ui_pump(void) {
    ui_lock();
    const std::uint64_t now = Board::now_ms();
    board.poll_input();
    bridge.key_snapshot(board.keys(), now);
    bridge.tick(now);

    Model& model = bridge.model();
    static bool sof_enabled = false;
    bool mounted = tud_mounted() && !usb_suspended;
    if (mounted && !sof_enabled) {
        tud_sof_cb_enable(true);
        sof_enabled = true;
    }
    if (mounted && usb_sof_seen) {
        const uint32_t now32 = static_cast<uint32_t>(now);
        if (now32 - usb_sof_ms > 100) {
            // On the Cardputer ADV there is no VBUS monitor. TinyUSB only
            // reports umount when the host drives a disconnect; poll SOF so
            // pulling the cable also clears the connected state.
            mounted = false;
        }
    }
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
    } else if (model.settings().volume != board.volume() ||
               model.settings().brightness != board.brightness() ||
               model.settings().timeout_index != applied_settings.timeout_index) {
        apply_settings(model.settings());
    }
    board.flush_input_feedback();

    if (!is_busy()) {
        pump_credentials();
    }

    if (power_off_failed) {
        power_off_failed = false;
        power_off_started = false;
        model.set_status("Power off failed", Board::now_ms());
        board.play(Tone::Failure);
    }

    board.render(model.view());
    ui_unlock();
}

extern "C" std::uint32_t cardputer_presence_begin(std::uint32_t timeout_ms,
                                                   std::uint8_t operation) {
    ui_lock();
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
    const std::uint32_t generation =
        bridge.begin(kind, rp, rp_len, account, account_len, Board::now_ms(),
                     timeout_ms != 0 ? timeout_ms : 30000);
    board.play(Tone::Request);
    ui_unlock();
    return generation;
}

extern "C" int cardputer_presence_take(std::uint32_t generation) {
    ui_lock();
    const PresenceResult result = bridge.take(generation);
    ui_unlock();
    switch (result) {
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

extern "C" void cardputer_presence_cancel(std::uint32_t generation) {
    ui_lock();
    bridge.cancel(generation, Board::now_ms());
    ui_unlock();
}

extern "C" std::uint32_t cardputer_presence_active(void) {
    ui_lock();
    const std::uint32_t generation = bridge.active_generation();
    ui_unlock();
    return generation;
}

extern "C" void cardputer_presence_set_kind(int kind) {
    ui_lock();
    presence_kind_override = kind;
    ui_unlock();
}

extern "C" void cardputer_storage_failure(int status) {
    (void)status;
    board.show_message("Storage error", "The key store could not be read");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" int picokey_board_init(void) {
    cardputer_ui_init();
    return 0;
}

extern "C" void picokey_ui_task(void) {
    cardputer_ui_pump();
}

extern "C" void picokey_storage_failure(int status) {
    cardputer_storage_failure(status);
}

extern "C" void tud_mount_cb(void) {
    usb_suspended = false;
}

extern "C" void tud_resume_cb(void) {
    usb_suspended = false;
}

extern "C" void tud_umount_cb(void) {
    usb_suspended = true;
    usb_sof_seen = false;
    cardputer_presence_cancel(cardputer_presence_active());
}

extern "C" void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    usb_suspended = true;
    usb_sof_seen = false;
    cardputer_presence_cancel(cardputer_presence_active());
}

extern "C" void tud_sof_cb(uint32_t frame_count) {
    (void)frame_count;
    usb_sof_ms = static_cast<uint32_t>(Board::now_ms());
    usb_sof_seen = true;
}
