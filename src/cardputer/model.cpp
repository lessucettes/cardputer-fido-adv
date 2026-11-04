#include "model.h"
#include <cstdio>
#include <cstring>

namespace cardputer {
namespace {
const char* const kMainItems[3] = {"Saved Keys", "Settings", "Power Off"};
const char* const kMainFooter = "Enter: Select  Del: Back";
const char* const kRequestFooter = "Enter: Confirm  Del: Cancel";
const char* const kEditFooter = "Left/Right: Change  Enter: Save";
constexpr int kMainCount = 3;

bool pressed(bool now, bool before) {
    return now && !before;
}

void copy_text(char* out, std::size_t cap, const char* text) {
    if (cap == 0) {
        return;
    }
    std::size_t i = 0;
    if (text != nullptr) {
        for (; text[i] != '\0' && i + 1 < cap; ++i) {
            out[i] = text[i];
        }
    }
    out[i] = '\0';
}

int clamp_index(int value, int count) {
    if (count <= 0) {
        return 0;
    }
    if (value < 0) {
        return 0;
    }
    if (value >= count) {
        return count - 1;
    }
    return value;
}
}  // namespace

void Model::start(std::uint64_t now) {
    last_activity_ = now;
    request_start_ = now;
    previous_ = {};
    screen_on_ = true;
    blocked_until_release_ = false;
    pending_ = false;
    decision_ = Decision::None;
    decision_generation_ = 0;
    active_generation_ = 0;
    screen_ = Screen::MainMenu;
    main_index_ = 0;
    key_index_ = 0;
    settings_index_ = 0;
    edit_field_ = 0;
    saved_key_count_ = 0;
    action_ = {};
    status_[0] = '\0';
}

void Model::set_screen_timeout(std::uint64_t milliseconds) {
    screen_timeout_ = milliseconds;
}

void Model::note_activity(std::uint64_t now) {
    last_activity_ = now;
}

void Model::tick(std::uint64_t now) {
    if (pending_ && now - request_start_ >= request_timeout_) {
        finish(Decision::TimedOut, now);
    }
    if (!pending_ && screen_timeout_ != 0 &&
        now - last_activity_ >= screen_timeout_) {
        screen_on_ = false;
    }
}

void Model::set_settings(const Settings& settings) {
    settings_ = settings;
    draft_ = settings;
    if (screen_timeout_ != screen_timeouts_ms[settings.timeout_index]) {
        screen_timeout_ = screen_timeouts_ms[settings.timeout_index];
    }
}

void Model::set_saved_keys(const SavedKey* keys, int count) {
    if (count < 0) {
        count = 0;
    }
    if (count > kMaxSavedKeys) {
        count = kMaxSavedKeys;
    }
    saved_key_count_ = count;
    if (keys != nullptr) {
        for (int i = 0; i < saved_key_count_; ++i) {
            saved_keys_[i] = keys[i];
        }
    } else {
        saved_key_count_ = 0;
    }
    key_index_ = clamp_index(key_index_, saved_key_count_);
    if (screen_ == Screen::KeyDetails || screen_ == Screen::ConfirmDeleteKey) {
        if (saved_key_count_ == 0) {
            screen_ = Screen::SavedKeys;
        }
    }
}

const SavedKey* Model::saved_key(int index) const {
    if (index < 0 || index >= saved_key_count_) {
        return nullptr;
    }
    return &saved_keys_[index];
}

void Model::set_usb_mounted(bool mounted) {
    usb_mounted_ = mounted;
}

void Model::set_status(const char* text) {
    copy_text(status_, sizeof(status_), text);
}

bool Model::take_action(Action* out) {
    if (action_.type == ActionType::None || out == nullptr) {
        return false;
    }
    *out = action_;
    action_ = {};
    return true;
}

void Model::key_snapshot(Keys keys, std::uint64_t now) {
    // Callers must supply the complete physical state, including modifiers.
    keys.any = keys.any || keys.enter || keys.del || keys.up || keys.down ||
               keys.left || keys.right || keys.modifier;
    if (keys.any) {
        note_activity(now);
    }
    if (!screen_on_ && keys.any) {
        screen_on_ = true;
        blocked_until_release_ = true;
    }
    if (blocked_until_release_) {
        previous_ = keys;
        if (!keys.any) {
            blocked_until_release_ = false;
        }
        return;
    }
    if (pending_) {
        // Cancellation wins when Enter and Del arrive in the same snapshot.
        if (pressed(keys.del, previous_.del)) {
            finish(Decision::Cancel, now);
        } else if (pressed(keys.enter, previous_.enter)) {
            finish(Decision::Confirm, now);
        }
        previous_ = keys;
        return;
    }
    navigate(keys);
    previous_ = keys;
}

void Model::navigate(Keys keys) {
    const bool up = pressed(keys.up, previous_.up);
    const bool down = pressed(keys.down, previous_.down);
    const bool left = pressed(keys.left, previous_.left);
    const bool right = pressed(keys.right, previous_.right);
    const bool enter = pressed(keys.enter, previous_.enter);
    const bool del = pressed(keys.del, previous_.del);

    switch (screen_) {
        case Screen::MainMenu:
            if (up) {
                main_index_ = (main_index_ + kMainCount - 1) % kMainCount;
            } else if (down) {
                main_index_ = (main_index_ + 1) % kMainCount;
            } else if (enter) {
                switch (main_index_) {
                    case 0:
                        screen_ = Screen::SavedKeys;
                        action_.type = ActionType::OpenSavedKeys;
                        break;
                    case 1:
                        screen_ = Screen::Settings;
                        break;
                    default:
                        action_.type = ActionType::PowerOff;
                        break;
                }
            }
            break;

        case Screen::SavedKeys:
            if (saved_key_count_ > 0) {
                if (up) {
                    key_index_ = (key_index_ + saved_key_count_ - 1) % saved_key_count_;
                } else if (down) {
                    key_index_ = (key_index_ + 1) % saved_key_count_;
                } else if (enter) {
                    screen_ = Screen::KeyDetails;
                }
            }
            if (del) {
                screen_ = Screen::MainMenu;
            }
            break;

        case Screen::KeyDetails:
            if (enter) {
                screen_ = Screen::ConfirmDeleteKey;
            } else if (del) {
                screen_ = Screen::SavedKeys;
            }
            break;

        case Screen::ConfirmDeleteKey: {
            const SavedKey* key = saved_key(key_index_);
            if (enter && key != nullptr) {
                action_.type = ActionType::DeleteKey;
                action_.key = *key;
                screen_ = Screen::KeyDetails;
            } else if (del) {
                screen_ = Screen::KeyDetails;
            }
            break;
        }

        case Screen::Settings:
            if (up) {
                settings_index_ = (settings_index_ + 2) % 3;
            } else if (down) {
                settings_index_ = (settings_index_ + 1) % 3;
            } else if (enter) {
                draft_ = settings_;
                edit_field_ = settings_index_;
                screen_ = Screen::SettingsEdit;
            } else if (del) {
                screen_ = Screen::MainMenu;
            }
            break;

        case Screen::SettingsEdit:
            if (left) {
                adjust_setting(-1);
            } else if (right) {
                adjust_setting(1);
            } else if (enter) {
                settings_ = draft_;
                set_screen_timeout(screen_timeouts_ms[settings_.timeout_index]);
                action_.type = ActionType::SaveSettings;
                action_.settings = settings_;
                screen_ = Screen::Settings;
            } else if (del) {
                draft_ = settings_;
                screen_ = Screen::Settings;
            }
            break;
    }
}

void Model::adjust_setting(int direction) {
    switch (edit_field_) {
        case 0:
            if (direction < 0 && draft_.volume > 0) {
                draft_.volume = static_cast<std::uint8_t>(draft_.volume - 10);
            } else if (direction > 0 && draft_.volume < 100) {
                draft_.volume = static_cast<std::uint8_t>(draft_.volume + 10);
            }
            break;
        case 1:
            if (direction < 0 && draft_.brightness > 10) {
                draft_.brightness = static_cast<std::uint8_t>(draft_.brightness - 10);
            } else if (direction > 0 && draft_.brightness < 100) {
                draft_.brightness = static_cast<std::uint8_t>(draft_.brightness + 10);
            }
            break;
        default:
            if (direction < 0 && draft_.timeout_index > 0) {
                draft_.timeout_index--;
            } else if (direction > 0 && static_cast<std::size_t>(draft_.timeout_index) + 1 <
                                          screen_timeouts_ms.size()) {
                draft_.timeout_index++;
            }
            break;
    }
}

void Model::set_line(Line* line, const char* text, bool selected) const {
    copy_text(line->text, sizeof(line->text), text);
    line->selected = selected;
}

Snapshot Model::view() const {
    Snapshot out;
    out.screen_on = screen_on_;
    out.status = status_[0] != '\0'
                     ? status_
                     : (usb_mounted_ ? "Waiting for a request"
                                     : "Connect USB to a computer");
    if (!screen_on_) {
        return out;
    }
    if (pending_) {
        switch (request_.kind) {
            case RequestKind::MakeCredential:
                out.title = "Create passkey?";
                break;
            case RequestKind::GetAssertion:
                out.title = "Sign in?";
                break;
            case RequestKind::Exclusion:
                out.title = "Check existing key?";
                break;
            case RequestKind::Reset:
                out.title = request_.stage >= 2 ? "Confirm deletion" : "Delete all keys?";
                break;
            case RequestKind::Selection:
                out.title = "Select an account";
                break;
            default:
                out.title = "Confirm security key?";
                break;
        }
        out.row_count = 0;
        if (request_.rp_id[0] != '\0') {
            set_line(&out.rows[out.row_count++], request_.rp_id, false);
        }
        if (request_.account[0] != '\0') {
            set_line(&out.rows[out.row_count++], request_.account, false);
        }
        if (request_.kind == RequestKind::Reset && request_.stage >= 2) {
            set_line(&out.rows[out.row_count++], "This cannot be undone", false);
        }
        out.footer = kRequestFooter;
        return out;
    }

    switch (screen_) {
        case Screen::MainMenu:
            out.title = "Cardputer FIDO";
            out.row_count = kMainCount;
            for (int i = 0; i < kMainCount; ++i) {
                set_line(&out.rows[i], kMainItems[i], i == main_index_);
            }
            out.footer = kMainFooter;
            break;
        case Screen::SavedKeys:
            out.title = "Saved Keys";
            if (saved_key_count_ == 0) {
                set_line(&out.rows[0], "No saved keys", false);
                out.row_count = 1;
            } else {
                const int visible = 3;
                int first = key_index_ - visible + 1;
                if (first < 0) {
                    first = 0;
                }
                if (first > saved_key_count_ - visible && saved_key_count_ > visible) {
                    first = saved_key_count_ - visible;
                }
                for (int i = 0; i < visible && first + i < saved_key_count_; ++i) {
                    const SavedKey& key = saved_keys_[first + i];
                    const char* name = key.protected_key
                                           ? "Protected key"
                                           : (key.account[0] != '\0' ? key.account
                                                                    : "Unnamed account");
                    set_line(&out.rows[out.row_count++], name, first + i == key_index_);
                }
            }
            out.footer = "Enter: Open  Del: Back";
            break;
        case Screen::KeyDetails: {
            out.title = "Saved Key";
            const SavedKey* key = saved_key(key_index_);
            if (key != nullptr) {
                set_line(&out.rows[out.row_count++], key->rp_id, false);
                set_line(&out.rows[out.row_count++],
                         key->protected_key
                             ? "Protected key"
                             : (key->account[0] != '\0' ? key->account : "Unnamed account"),
                         false);
                set_line(&out.rows[out.row_count++], "Delete Key", true);
            }
            out.footer = "Enter: Delete  Del: Back";
            break;
        }
        case Screen::ConfirmDeleteKey: {
            out.title = "Delete key?";
            const SavedKey* key = saved_key(key_index_);
            if (key != nullptr) {
                set_line(&out.rows[out.row_count++], key->rp_id, false);
                set_line(&out.rows[out.row_count++],
                         key->account[0] != '\0' ? key->account : "Unnamed account", false);
            }
            out.footer = kRequestFooter;
            break;
        }
        case Screen::Settings:
            out.title = "Settings";
            out.row_count = 3;
            {
                char buffer[48];
                std::snprintf(buffer, sizeof(buffer), "Volume  %u", settings_.volume);
                set_line(&out.rows[0], buffer, settings_index_ == 0);
                std::snprintf(buffer, sizeof(buffer), "Brightness  %u", settings_.brightness);
                set_line(&out.rows[1], buffer, settings_index_ == 1);
                std::snprintf(buffer, sizeof(buffer), "Screen Timeout  %s",
                              screen_timeout_labels[settings_.timeout_index]);
                set_line(&out.rows[2], buffer, settings_index_ == 2);
            }
            out.footer = kMainFooter;
            break;
        case Screen::SettingsEdit: {
            static const char* const labels[3] = {"Volume", "Brightness", "Screen Timeout"};
            out.title = labels[edit_field_];
            out.row_count = 1;
            char buffer[48];
            if (edit_field_ == 0) {
                std::snprintf(buffer, sizeof(buffer), "Volume  %u", draft_.volume);
            } else if (edit_field_ == 1) {
                std::snprintf(buffer, sizeof(buffer), "Brightness  %u", draft_.brightness);
            } else {
                std::snprintf(buffer, sizeof(buffer), "Screen Timeout  %s",
                              screen_timeout_labels[draft_.timeout_index]);
            }
            set_line(&out.rows[0], buffer, true);
            out.footer = kEditFooter;
            break;
        }
    }
    return out;
}

std::uint32_t Model::begin_request(std::uint64_t now, std::uint64_t timeout_ms) {
    RequestInfo info;
    return begin_request(now, timeout_ms, info);
}

std::uint32_t Model::begin_request(std::uint64_t now, std::uint64_t timeout_ms,
                                   const RequestInfo& info) {
    if (pending_) {
        finish(Decision::Cancel, now);
    }
    request_ = info;
    request_.rp_id[sizeof(request_.rp_id) - 1] = '\0';
    request_.account[sizeof(request_.account) - 1] = '\0';
    active_generation_ = next_generation_++;
    if (next_generation_ == 0) {
        next_generation_ = 1;
    }
    pending_ = true;
    screen_on_ = true;
    decision_ = Decision::None;
    decision_generation_ = 0;
    request_start_ = now;
    request_timeout_ = timeout_ms;
    last_activity_ = now;
    blocked_until_release_ = blocked_until_release_ || previous_.any;
    return active_generation_;
}

void Model::finish(Decision decision, std::uint64_t now) {
    pending_ = false;
    decision_ = decision;
    decision_generation_ = active_generation_;
    last_activity_ = now;
}

void Model::cancel_request(std::uint64_t now) {
    if (pending_) {
        finish(Decision::Cancel, now);
    }
}

Decision Model::take_decision() {
    const auto result = decision_;
    decision_ = Decision::None;
    return result;
}

}  // namespace cardputer
