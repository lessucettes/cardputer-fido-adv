#include "model.h"
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

static Keys key_enter() {
    Keys keys;
    keys.any = true;
    keys.enter = true;
    return keys;
}

static Keys key_del() {
    Keys keys;
    keys.any = true;
    keys.del = true;
    return keys;
}

static Keys key_down() {
    Keys keys;
    keys.any = true;
    keys.down = true;
    return keys;
}

static Keys key_up() {
    Keys keys;
    keys.any = true;
    keys.up = true;
    return keys;
}

static Keys key_right() {
    Keys keys;
    keys.any = true;
    keys.right = true;
    return keys;
}

static Keys key_left() {
    Keys keys;
    keys.any = true;
    keys.left = true;
    return keys;
}

static void tap(Model& model, Keys keys, std::uint64_t now) {
    model.key_snapshot(keys, now);
    model.key_snapshot(Keys{}, now + 1);
}

static void test_appendix_a() {
    Model m;
    m.start(0);
    m.set_screen_timeout(100);
    m.tick(100);
    require(!m.screen_on(), "screen should time out");
    m.key_snapshot({true, true, false}, 101);
    require(m.screen_on(), "any key should wake screen");
    m.begin_request(102, 1000);
    m.key_snapshot({true, true, false}, 103);
    require(m.take_decision() == Decision::None,
            "waking Enter must not approve");
    m.key_snapshot({}, 104);
    m.key_snapshot({true, true, false}, 105);
    require(m.take_decision() == Decision::Confirm,
            "fresh Enter should confirm");
    require(m.take_decision() == Decision::None,
            "decision must be consumed exactly once");

    m.begin_request(106, 1000);
    m.key_snapshot({true, true, false}, 107);
    require(m.take_decision() == Decision::None,
            "held Enter must not approve a new request");
    m.key_snapshot({}, 108);
    m.key_snapshot({true, false, true}, 109);
    require(m.take_decision() == Decision::Cancel, "Del should cancel");

    m.key_snapshot({}, 110);
    m.begin_request(111, 10);
    m.tick(121);
    require(m.take_decision() == Decision::TimedOut,
            "request should time out");

    m.begin_request(122, 10);
    m.cancel_request(123);
    m.key_snapshot({true, true, false}, 124);
    require(m.take_decision() == Decision::Cancel,
            "host cancellation must not turn into approval");

    m.key_snapshot({}, 125);
    m.set_screen_timeout(0);
    m.tick(1000000);
    require(m.screen_on(), "Never must keep screen on");
    m.set_screen_timeout(10);
    m.begin_request(1000001, 100);
    m.tick(1000011);
    require(m.screen_on(), "pending prompt must keep screen on");

    m.cancel_request(1000012);
    (void)m.take_decision();
    m.tick(1000022);
    require(!m.screen_on(), "idle screen should turn off again");
    m.key_snapshot({true, false, false}, 1000023);
    require(m.screen_on(), "modifier-only key should wake");
    m.key_snapshot({true, true, false}, 1000024);
    m.begin_request(1000025, 100);
    m.key_snapshot({true, true, false}, 1000026);
    require(m.take_decision() == Decision::None,
            "a wake chord must stay blocked until complete release");
}

static void test_main_menu() {
    Model m;
    m.start(0);
    Snapshot v = m.view();
    require(std::strcmp(v.title, "Cardputer FIDO") == 0, "main title");
    require(v.row_count == 3, "main menu has three items");
    require(std::strcmp(v.rows[0].text, "Saved Keys") == 0, "first item");
    require(std::strcmp(v.rows[1].text, "Settings") == 0, "second item");
    require(std::strcmp(v.rows[2].text, "Power Off") == 0, "third item");
    require(v.rows[0].selected && !v.rows[1].selected, "default selection");

    tap(m, key_down(), 100);
    v = m.view();
    require(v.rows[1].selected, "down selects Settings");
    tap(m, key_up(), 200);
    v = m.view();
    require(v.rows[0].selected, "up returns to Saved Keys");

    tap(m, key_up(), 300);
    v = m.view();
    require(v.rows[2].selected, "up wraps to Power Off");
    tap(m, key_enter(), 400);
    Action action;
    require(m.take_action(&action), "Power Off emits an action");
    require(action.type == ActionType::PowerOff, "action is Power Off");
    require(std::strcmp(m.view().title, "Cardputer FIDO") == 0,
            "Power Off does not change to Launcher");
}

static void test_saved_keys() {
    Model m;
    m.start(0);
    tap(m, key_enter(), 100);
    Action list_action;
    require(m.take_action(&list_action), "entering Saved Keys requests the list");
    require(list_action.type == ActionType::OpenSavedKeys,
            "saved keys entry emits a list action");
    Snapshot v = m.view();
    require(std::strcmp(v.title, "Saved Keys") == 0, "saved keys title");
    require(v.row_count == 1 && std::strcmp(v.rows[0].text, "No saved keys") == 0,
            "empty saved keys text");

    SavedKey keys[2] = {};
    std::strcpy(keys[0].rp_id, "example.com");
    std::strcpy(keys[0].account, "alice");
    keys[0].id_len = 4;
    keys[0].id[0] = 1;
    std::strcpy(keys[1].rp_id, "example.org");
    keys[1].protected_key = true;
    keys[1].id_len = 4;
    keys[1].id[1] = 2;
    m.set_saved_keys(keys, 2);
    v = m.view();
    require(v.row_count == 2, "two saved keys visible");
    require(std::strcmp(v.rows[0].text, "alice") == 0, "account shown");
    require(std::strcmp(v.rows[1].text, "Protected key") == 0,
            "protected key placeholder");

    tap(m, key_enter(), 200);
    v = m.view();
    require(std::strcmp(v.title, "Saved Key") == 0, "details title");
    require(std::strcmp(v.rows[0].text, "example.com") == 0, "details RP");
    require(std::strcmp(v.rows[2].text, "Delete Key") == 0, "delete action");

    tap(m, key_enter(), 300);
    v = m.view();
    require(std::strcmp(v.title, "Delete key?") == 0, "delete confirmation");
    Action action;
    require(!m.take_action(&action), "confirmation alone does not delete");
    tap(m, key_enter(), 400);
    require(m.take_action(&action), "second Enter requests deletion");
    require(action.type == ActionType::DeleteKey, "delete action type");
    require(std::strcmp(action.key.rp_id, "example.com") == 0, "delete identity");

    tap(m, key_enter(), 500);
    tap(m, key_enter(), 600);
    tap(m, key_del(), 700);
    v = m.view();
    require(std::strcmp(v.title, "Saved Keys") == 0, "Del returns to list");
}

static void test_saved_keys_boundary() {
    Model m;
    m.start(0);
    SavedKey keys[Model::kMaxSavedKeys] = {};
    m.set_saved_keys(keys, Model::kMaxSavedKeys);
    require(m.saved_key_count() == Model::kMaxSavedKeys,
            "all 32 saved keys are retained");
    m.set_saved_keys(keys, Model::kMaxSavedKeys + 1);
    require(m.saved_key_count() == Model::kMaxSavedKeys,
            "saved-key count is bounded");
}

static void test_settings() {
    Model m;
    m.start(0);
    m.set_settings(Settings{0, 60, 2});
    tap(m, key_down(), 100);
    tap(m, key_enter(), 200);
    Snapshot v = m.view();
    require(std::strcmp(v.title, "Settings") == 0, "settings screen");
    require(v.row_count == 3, "three settings");
    require(std::strstr(v.rows[0].text, "Volume") != nullptr, "volume row");
    require(std::strstr(v.rows[1].text, "Brightness") != nullptr, "brightness row");
    require(std::strstr(v.rows[2].text, "Screen Timeout") != nullptr, "timeout row");

    tap(m, key_enter(), 300);
    v = m.view();
    require(std::strcmp(v.title, "Volume") == 0, "volume editor");
    tap(m, key_left(), 350);
    require(m.draft_settings().volume == 0, "left clamps volume at zero");
    tap(m, key_right(), 400);
    require(m.draft_settings().volume == 10, "right previews volume");
    Action action;
    require(!m.take_action(&action), "preview is not saved");
    tap(m, key_del(), 500);
    require(m.settings().volume == 0, "Del restores entry value");
    require(m.draft_settings().volume == 0, "draft restored");

    tap(m, key_enter(), 600);
    tap(m, key_right(), 700);
    tap(m, key_enter(), 800);
    require(m.take_action(&action), "Enter saves settings");
    require(action.type == ActionType::SaveSettings, "save action type");
    require(action.settings.volume == 10, "saved volume");

    tap(m, key_del(), 900);
    v = m.view();
    require(std::strcmp(v.title, "Cardputer FIDO") == 0, "Del leaves settings");
}

static void test_request_overlay() {
    Model m;
    m.start(0);
    RequestInfo info;
    info.kind = RequestKind::MakeCredential;
    std::strcpy(info.rp_id, "example.com");
    std::strcpy(info.account, "alice");
    const std::uint32_t generation = m.begin_request(100, 1000, info);
    Snapshot v = m.view();
    require(std::strcmp(v.title, "Create passkey?") == 0, "create title");
    require(v.row_count == 2, "RP and account rows");
    require(std::strcmp(v.rows[0].text, "example.com") == 0, "RP row");
    require(std::strcmp(v.rows[1].text, "alice") == 0, "account row");
    require(std::strcmp(v.footer, "ENT:YES DEL:NO") == 0,
            "request footer");

    tap(m, key_enter(), 200);
    require(m.take_decision() == Decision::Confirm, "confirm decision");
    require(m.decision_generation() == generation, "decision carries generation");

    v = m.view();
    require(v.title != nullptr && std::strcmp(v.title, "Cardputer FIDO") == 0,
            "overlay clears after decision");
    require(!m.pending(), "request no longer pending");
}

static void test_screen_off_snapshot() {
    Model m;
    m.start(0);
    m.set_screen_timeout(10);
    m.tick(20);
    require(!m.screen_on(), "screen off");
    Snapshot v = m.view();
    require(!v.screen_on, "snapshot reports screen off");
    require(v.row_count == 0, "no rows while off");
}

int main() {
    test_appendix_a();
    test_main_menu();
    test_saved_keys();
    test_saved_keys_boundary();
    test_settings();
    test_request_overlay();
    test_screen_off_snapshot();
    std::puts("PASS: keyboard wake, menu, settings and request decisions");
    return 0;
}
