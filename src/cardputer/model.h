#pragma once
#include <cstdint>
#include "settings.h"

namespace cardputer {

enum class Decision { None, Confirm, Cancel, TimedOut };
enum class RequestKind { Generic, MakeCredential, GetAssertion, Exclusion, Reset, Selection };
enum class Screen { MainMenu, SavedKeys, KeyDetails, Settings, SettingsEdit, ConfirmDeleteKey };
enum class ActionType { None, OpenSavedKeys, OpenSettings, PowerOff, DeleteKey, SaveSettings };

struct Keys {
    bool any = false;
    bool enter = false;
    bool del = false;
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool modifier = false;
};

struct RequestInfo {
    RequestKind kind = RequestKind::Generic;
    char rp_id[256] = {};
    char account[65] = {};
    std::uint8_t stage = 0;
};

struct SavedKey {
    std::uint8_t id[32] = {};
    std::uint8_t id_len = 0;
    char rp_id[256] = {};
    char account[65] = {};
    bool protected_key = false;
};

struct Line {
    char text[48] = {};
    bool selected = false;
};

struct Snapshot {
    const char* title = "";
    Line rows[4] = {};
    int row_count = 0;
    const char* footer = "";
    const char* status = "";
    bool screen_on = true;
};

struct Action {
    ActionType type = ActionType::None;
    SavedKey key = {};
    Settings settings = {};
};

class Model {
public:
    static constexpr int kMaxSavedKeys = 32;

    void start(std::uint64_t now);
    void set_screen_timeout(std::uint64_t milliseconds);
    std::uint64_t screen_timeout() const { return screen_timeout_; }
    void tick(std::uint64_t now);
    void key_snapshot(Keys keys, std::uint64_t now);

    std::uint32_t begin_request(std::uint64_t now, std::uint64_t timeout_ms);
    std::uint32_t begin_request(std::uint64_t now, std::uint64_t timeout_ms,
                                const RequestInfo& info);
    void cancel_request(std::uint64_t now);
    Decision take_decision();
    std::uint32_t decision_generation() const { return decision_generation_; }
    std::uint32_t active_generation() const { return active_generation_; }
    const RequestInfo* active_request() const { return pending_ ? &request_ : nullptr; }

    bool screen_on() const { return screen_on_; }
    bool pending() const { return pending_; }
    Screen screen() const { return screen_; }
    const Settings& settings() const { return settings_; }
    const Settings& draft_settings() const { return draft_; }
    bool editing_settings() const { return screen_ == Screen::SettingsEdit; }

    void set_settings(const Settings& settings);
    void set_saved_keys(const SavedKey* keys, int count);
    int saved_key_count() const { return saved_key_count_; }
    const SavedKey* saved_key(int index) const;
    void set_usb_mounted(bool mounted);
    bool usb_mounted() const { return usb_mounted_; }
    void set_status(const char* text);

    bool take_action(Action* out);
    Snapshot view() const;

private:
    void finish(Decision decision, std::uint64_t now);
    void navigate(Keys keys);
    void adjust_setting(int direction);
    void note_activity(std::uint64_t now);
    void set_line(Line* line, const char* text, bool selected) const;

    std::uint64_t last_activity_ = 0;
    std::uint64_t screen_timeout_ = 60000;
    std::uint64_t request_start_ = 0;
    std::uint64_t request_timeout_ = 30000;
    Keys previous_{};
    bool screen_on_ = true;
    bool blocked_until_release_ = false;
    bool pending_ = false;
    bool usb_mounted_ = false;
    Decision decision_ = Decision::None;
    std::uint32_t next_generation_ = 1;
    std::uint32_t active_generation_ = 0;
    std::uint32_t decision_generation_ = 0;
    RequestInfo request_{};
    Screen screen_ = Screen::MainMenu;
    int main_index_ = 0;
    int key_index_ = 0;
    int settings_index_ = 0;
    int edit_field_ = 0;
    Settings settings_{};
    Settings draft_{};
    SavedKey saved_keys_[kMaxSavedKeys] = {};
    int saved_key_count_ = 0;
    Action action_{};
    char status_[64] = {};
};

}  // namespace cardputer
