/*
 * Host harness for src/cardputer/backup.cpp maintenance orchestration.
 *
 * The production file is compiled verbatim (a byte copy with its real
 * backup.h/board.h) against narrow ESP-IDF shims. Only the hardware boundary
 * is replaced: NVS, the fido_store partition, SPI/SD mount and the board keys /
 * screen. Startup routing, file-generation preservation and restore guard
 * ordering therefore run as the real functions, not as extracted snippets.
 *
 * Every scenario runs in its own process so backup.cpp's static "mounted" flag
 * and the boot-scoped NVS state behave like one power cycle.
 */
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <functional>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "backup.h"
#include "backup_codec.h"
#include "backup_store.h"
#include "flash_layout.h"
#include "key_store.h"

extern "C" {
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

uint8_t pico_serial_hash[32] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F,
};
}

using cardputer::BackupMode;
using cardputer::Board;
using cardputer::Keys;
using cardputer::Tone;

namespace {

/* ------------------------------------------------------------------ */
/* Failures and process-wide recording                                 */
/* ------------------------------------------------------------------ */

struct Failure {
    std::string message;
};
struct Stop {
    std::string tag;
};
struct Restart {};

[[noreturn]] void failf(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    throw Failure{buffer};
}

void require(bool condition, const char* what) {
    if (!condition) { failf("assertion failed: %s", what); }
}

std::string format(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return std::string(buffer);
}

std::vector<std::string> events;
std::vector<std::string> titles;
std::vector<std::string> row_texts;
std::string last_title;
std::string test_root;

void event(const std::string& text) { events.push_back(text); }

long first_event(const std::string& text) {
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i] == text) { return static_cast<long>(i); }
    }
    return -1;
}

long first_event_prefix(const std::string& prefix) {
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].compare(0, prefix.size(), prefix) == 0) {
            return static_cast<long>(i);
        }
    }
    return -1;
}

bool saw_title(const std::string& title) {
    return std::find(titles.begin(), titles.end(), title) != titles.end();
}

bool saw_text(const std::string& text) {
    return std::find(row_texts.begin(), row_texts.end(), text) != row_texts.end();
}

/* ------------------------------------------------------------------ */
/* NVS shim                                                            */
/* ------------------------------------------------------------------ */

std::map<std::string, uint8_t> nvs_values;
bool nvs_namespace_present = false;
int nvs_erase_calls = 0;
int nvs_fail_init = 0;
int nvs_fail_set_times = 0;
std::string nvs_fail_set_key;
int nvs_fail_set_value = -1;
std::string nvs_fail_get_key;
int nvs_fail_commit_times = 0;

int nvs_read(const std::string& key) {
    if (!nvs_namespace_present) { return -1; }
    const auto it = nvs_values.find(key);
    return it == nvs_values.end() ? -1 : static_cast<int>(it->second);
}

void nvs_put(const std::string& key, int value) {
    nvs_namespace_present = true;
    nvs_values[key] = static_cast<uint8_t>(value);
}

/* ------------------------------------------------------------------ */
/* fido_store partition shim                                           */
/* ------------------------------------------------------------------ */

esp_partition_t partition = {0x210000u, CF_FIDO_STORE_SIZE};
std::vector<uint8_t> store_bytes(CF_FIDO_STORE_SIZE, 0xFF);
long partition_reads = 0;
long partition_writes = 0;
long partition_erases = 0;
long fail_read_call = -1;
long fail_write_call = -1;
long fail_erase_call = -1;
long erase0_calls = 0;
long key_read_corrupt_after_erase0 = 0;
long last_key_read_event = -1;
int prepare_result = 0;
int prepare_calls = 0;

/* ------------------------------------------------------------------ */
/* SPI / SD mount shim                                                 */
/* ------------------------------------------------------------------ */

int spi_init_calls = 0;
int spi_free_calls = 0;
int sd_mount_calls = 0;
int sd_unmount_calls = 0;
int spi_init_result = ESP_OK;
int sd_mount_result = ESP_FAIL;
int sd_unmount_result = ESP_OK;
spi_host_device_t last_spi_host = -1;
spi_bus_config_t last_bus_config = {};
sdspi_device_config_t last_device_config = {};
sdmmc_host_t last_sd_host = {};
esp_vfs_fat_mount_config_t last_mount_config = {};
std::string last_mount_base;
sdmmc_card_t fake_card = {};
sdmmc_card_t* last_unmount_card = nullptr;

/* ------------------------------------------------------------------ */
/* Board shim                                                          */
/* ------------------------------------------------------------------ */

struct Step {
    Keys keys;
    std::string typed;
    bool fn_held = false;
    std::function<void()> action;
    std::string throw_tag;
    std::string expected_title;
    int id = 0;
};

std::deque<Step> steps;
long poll_count = 0;
long poll_limit = 20000;
std::uint64_t now_ms_counter = 0;
int restart_calls = 0;
int fsync_calls = 0;
bool fail_fsync = false;
bool corrupt_tmp_on_verify = false;
std::string fail_rename_from;
int rename_calls = 0;

std::string path_of(const std::string& name) {
    return test_root + "/" + name;
}

bool file_exists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> names;
    DIR* dir = opendir(path.c_str());
    if (!dir) { return names; }
    while (dirent* entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..") { names.push_back(name); }
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<uint8_t> file_bytes(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) { failf("cannot open %s", path.c_str()); }
    std::vector<uint8_t> data;
    uint8_t buffer[4096];
    std::size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        data.insert(data.end(), buffer, buffer + got);
    }
    std::fclose(file);
    return data;
}

/* ------------------------------------------------------------------ */
/* Board implementation (only the maintenance UI surface)              */
/* ------------------------------------------------------------------ */

}  // namespace

bool cardputer::Board::begin() { return true; }

void cardputer::Board::poll_input() {
    ++poll_count;
    if (poll_count > poll_limit) {
        throw Failure{"poll budget exhausted; last screen: " + last_title};
    }
    keys_ = Keys{};
    typed_[0] = '\0';
    fn_held_ = false;
    if (steps.empty()) { return; }
    Step step = std::move(steps.front());
    steps.pop_front();
    if (step.action) { step.action(); }
    keys_ = step.keys;
    std::snprintf(typed_, sizeof(typed_), "%s", step.typed.c_str());
    fn_held_ = step.fn_held;
    if (!step.expected_title.empty() && last_title != step.expected_title) {
        failf("expected screen '%s' but last screen was '%s'",
              step.expected_title.c_str(), last_title.c_str());
    }
    if (!step.throw_tag.empty()) { throw Stop{step.throw_tag}; }
}

std::uint64_t cardputer::Board::now_ms() { return ++now_ms_counter; }

void cardputer::Board::render(const cardputer::Snapshot& snapshot) {
    last_title = snapshot.title ? snapshot.title : "";
    titles.push_back(last_title);
    for (int i = 0; i < snapshot.row_count && i < 4; ++i) {
        row_texts.emplace_back(snapshot.rows[i].text);
    }
}

void cardputer::Board::show_message(const char* title, const char* detail) {
    (void)detail;
    last_title = title ? title : "";
    titles.push_back(last_title);
}

void cardputer::Board::set_brightness(std::uint8_t) {}
void cardputer::Board::set_volume(std::uint8_t) {}
void cardputer::Board::play(Tone) {}
void cardputer::Board::service_audio() {}
void cardputer::Board::flush_input_feedback() {}
void cardputer::Board::power_off() {}
void cardputer::Board::queue_input_feedback(Tone) {}

Board board;

namespace {

/* ------------------------------------------------------------------ */
/* C stubs                                                             */
/* ------------------------------------------------------------------ */

}  // namespace

extern "C" {

esp_err_t nvs_flash_init(void) {
    event("nvs.init");
    return nvs_fail_init ? ESP_FAIL : ESP_OK;
}

esp_err_t nvs_flash_erase(void) {
    ++nvs_erase_calls;
    event("nvs.erase");
    return ESP_OK;
}

esp_err_t nvs_open(const char*, nvs_open_mode_t mode, nvs_handle_t* out) {
    if (mode == NVS_READONLY && !nvs_namespace_present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t) {}

esp_err_t nvs_get_u8(nvs_handle_t, const char* key, uint8_t* out) {
    if (nvs_fail_get_key == key) { return ESP_FAIL; }
    if (!nvs_namespace_present) { return ESP_ERR_NVS_NOT_FOUND; }
    const auto it = nvs_values.find(key);
    if (it == nvs_values.end()) { return ESP_ERR_NVS_NOT_FOUND; }
    *out = it->second;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t, const char* key, uint8_t value) {
    if (nvs_fail_set_times > 0 && nvs_fail_set_key == key &&
        (nvs_fail_set_value < 0 || nvs_fail_set_value == value)) {
        --nvs_fail_set_times;
        event(format("nvs.set.fail:%s=%u", key, value));
        return ESP_FAIL;
    }
    nvs_namespace_present = true;
    nvs_values[key] = value;
    event(format("nvs.set:%s=%u", key, value));
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t) {
    if (nvs_fail_commit_times > 0) {
        --nvs_fail_commit_times;
        event("nvs.commit.fail");
        return ESP_FAIL;
    }
    event("nvs.commit");
    return ESP_OK;
}

esp_err_t esp_partition_read(const esp_partition_t* part, size_t offset,
                             void* out, size_t size) {
    ++partition_reads;
    if (part != &partition || offset + size > store_bytes.size()) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string record = format("partition.read:%zu:%zu", offset, size);
    event(record);
    if (offset < CF_FIDO_FS_OFFSET) { last_key_read_event = static_cast<long>(events.size()) - 1; }
    if (fail_read_call > 0 && partition_reads == fail_read_call) { return ESP_FAIL; }
    std::memcpy(out, store_bytes.data() + offset, size);
    if (key_read_corrupt_after_erase0 > 0 &&
        erase0_calls >= key_read_corrupt_after_erase0 &&
        offset < CF_FIDO_FS_OFFSET) {
        static_cast<uint8_t*>(out)[0] ^= 0x01;
    }
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t* part, size_t offset,
                              const void* src, size_t size) {
    ++partition_writes;
    if (part != &partition || offset + size > store_bytes.size()) {
        return ESP_ERR_INVALID_ARG;
    }
    event(format("partition.write:%zu:%zu", offset, size));
    if (fail_write_call > 0 && partition_writes == fail_write_call) {
        return ESP_FAIL;
    }
    const uint8_t* data = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < size; ++i) {
        store_bytes[offset + i] &= data[i];
    }
    return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t* part, size_t offset,
                                    size_t size) {
    ++partition_erases;
    if (part != &partition || offset + size > store_bytes.size()) {
        return ESP_ERR_INVALID_ARG;
    }
    event(format("partition.erase:%zu:%zu", offset, size));
    if (offset == 0 && size == CF_FIDO_FS_OFFSET) { ++erase0_calls; }
    if (fail_erase_call > 0 && partition_erases == fail_erase_call) {
        return ESP_FAIL;
    }
    std::memset(store_bytes.data() + offset, 0xFF, size);
    return ESP_OK;
}

void esp_fill_random(void* out, size_t len) {
    static uint8_t counter = 0x5A;
    uint8_t* bytes = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < len; ++i) {
        bytes[i] = counter++;
    }
}

void esp_restart(void) {
    ++restart_calls;
    event("restart");
    throw Restart{};
}

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t* config,
                             int dma) {
    ++spi_init_calls;
    last_spi_host = host;
    last_bus_config = *config;
    (void)dma;
    event("spi.init");
    return spi_init_result;
}

esp_err_t spi_bus_free(spi_host_device_t host) {
    (void)host;
    ++spi_free_calls;
    event("spi.free");
    return ESP_OK;
}

esp_err_t esp_vfs_fat_sdspi_mount(const char* base_path,
                                  const sdmmc_host_t* host,
                                  const sdspi_device_config_t* device,
                                  const esp_vfs_fat_mount_config_t* config,
                                  sdmmc_card_t** out_card) {
    ++sd_mount_calls;
    last_mount_base = base_path;
    last_sd_host = *host;
    last_device_config = *device;
    last_mount_config = *config;
    event("sd.mount");
    if (sd_mount_result != ESP_OK) { return sd_mount_result; }
    *out_card = &fake_card;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_sdcard_unmount(const char* base_path, sdmmc_card_t* card) {
    ++sd_unmount_calls;
    last_unmount_card = card;
    event("sd.unmount");
    if (base_path == nullptr || std::strcmp(base_path, "/sdcard") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sd_unmount_result != ESP_OK) { return sd_unmount_result; }
    return ESP_OK;
}

void vTaskDelay(TickType_t) {}

const esp_partition_t* cardputer_fido_partition(void) { return &partition; }

int cardputer_key_store_prepare(void) {
    ++prepare_calls;
    event("prepare");
    return prepare_result;
}

}  // extern "C"

namespace {

/* ------------------------------------------------------------------ */
/* Path wrapping: the production absolute mount paths stay unchanged.  */
/* ------------------------------------------------------------------ */

std::string map_path(const char* path) {
    const std::string value = path ? path : "";
    const std::string prefix = "/sdcard/";
    if (value.compare(0, prefix.size(), prefix) == 0) {
        return test_root + "/" + value.substr(prefix.size());
    }
    return value;
}

}  // namespace

extern "C" {

FILE* __real_fopen(const char*, const char*);
int __real_stat(const char*, struct stat*);
int __real_remove(const char*);
int __real_rename(const char*, const char*);
int __real_fsync(int);

FILE* __wrap_fopen(const char* path, const char* mode) {
    const std::string mapped = map_path(path);
    if (corrupt_tmp_on_verify && mode && mode[0] == 'r' &&
        mapped.size() >= 4 && mapped.compare(mapped.size() - 4, 4, ".TMP") == 0) {
        corrupt_tmp_on_verify = false;
        FILE* file = __real_fopen(mapped.c_str(), "rb");
        if (!file) { return nullptr; }
        std::vector<uint8_t> data;
        uint8_t buffer[4096];
        std::size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            data.insert(data.end(), buffer, buffer + got);
        }
        std::fclose(file);
        if (!data.empty()) { data[data.size() - 1] ^= 0x01; }
        file = __real_fopen(mapped.c_str(), "wb");
        if (!file) { return nullptr; }
        if (!data.empty()) { std::fwrite(data.data(), 1, data.size(), file); }
        std::fclose(file);
        return __real_fopen(mapped.c_str(), "rb");
    }
    return __real_fopen(mapped.c_str(), mode);
}

int __wrap_stat(const char* path, struct stat* info) {
    return __real_stat(map_path(path).c_str(), info);
}

int __wrap_remove(const char* path) {
    return __real_remove(map_path(path).c_str());
}

int __wrap_rename(const char* from, const char* to) {
    const std::string mapped_from = map_path(from);
    const std::string mapped_to = map_path(to);
    ++rename_calls;
    if (!fail_rename_from.empty() && fail_rename_from == mapped_from) {
        errno = EPERM;
        return -1;
    }
    return __real_rename(mapped_from.c_str(), mapped_to.c_str());
}

int __wrap_fsync(int fd) {
    ++fsync_calls;
    if (fail_fsync) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

}  // extern "C"

namespace {

/* ------------------------------------------------------------------ */
/* Scripted input                                                      */
/* ------------------------------------------------------------------ */

int next_step_id = 0;

void add(Step step) {
    step.id = ++next_step_id;
    steps.push_back(std::move(step));
}

void add_release() { add(Step{}); }

void add_press(Keys keys, std::function<void()> action = {}) {
    Step step;
    step.keys = keys;
    step.action = std::move(action);
    add(std::move(step));
}

void add_key_action(Keys keys, std::function<void()> action = {}) {
    add_press(keys);
    add_release();
    add_press(keys, std::move(action));
}

void add_enter(std::function<void()> action = {}) {
    Keys keys;
    keys.any = keys.enter = true;
    add_key_action(keys, std::move(action));
}

void add_delete(std::function<void()> action = {}) {
    Keys keys;
    keys.any = keys.del = true;
    add_key_action(keys, std::move(action));
}

void add_typed(const std::string& text) {
    Step step;
    step.typed = text;
    add(std::move(step));
}

void add_password(const std::string& password) {
    add_release();
    add_typed(password);
    Keys keys;
    keys.any = keys.enter = true;
    add_press(keys);
}

void add_notice_ok() { add_enter(); }

void add_stop(const std::string& tag, const std::string& expected_title = {}) {
    Step step;
    step.throw_tag = tag;
    step.expected_title = expected_title;
    add(std::move(step));
}

struct RunResult {
    enum class Kind { Returned, Restarted, Stopped } kind = Kind::Returned;
    std::string tag;
};

RunResult run_startup() {
    try {
        cardputer::backup_startup(board);
        return {RunResult::Kind::Returned, {}};
    } catch (const Restart&) {
        return {RunResult::Kind::Restarted, {}};
    } catch (const Stop& stop) {
        return {RunResult::Kind::Stopped, stop.tag};
    }
}

void require_stopped(const RunResult& result, const std::string& tag) {
    require(result.kind == RunResult::Kind::Stopped &&
            result.tag == tag, "expected stop tag");
}

void require_sd_released() {
    require(sd_unmount_calls == 1, "session unmounts the card exactly once");
    require(last_unmount_card == &fake_card, "the mounted card is unmounted");
    require(spi_free_calls == 1, "session releases the SPI bus");
}

/* ------------------------------------------------------------------ */
/* Store and backup-file helpers                                       */
/* ------------------------------------------------------------------ */

std::vector<uint8_t> make_store(uint8_t seed) {
    std::vector<uint8_t> store(CF_FIDO_STORE_SIZE, 0);
    for (std::size_t i = CF_FIDO_FS_OFFSET; i < store.size(); ++i) {
        store[i] = static_cast<uint8_t>((i * 37u + seed * 101u) & 0xFFu);
    }
    cf_device_keys keys = {};
    for (int i = 0; i < 32; ++i) {
        keys.key1[i] = static_cast<uint8_t>(i == 0 ? seed + 1 : i * 3 + seed);
        keys.key2[i] = static_cast<uint8_t>(i == 0 ? seed + 2 : i * 5 + seed + 7);
    }
    uint8_t record[CF_KEY_PAGE_SIZE];
    cf_key_record_encode(&keys, record);
    std::memcpy(store.data(), record, CF_KEY_PAGE_SIZE);
    std::memcpy(store.data() + CF_KEY_PAGE_SIZE, record, CF_KEY_PAGE_SIZE);
    return store;
}

struct ExportSource {
    const std::vector<uint8_t>* bytes;
};

int source_read(void* ctx, uint32_t offset, uint8_t* out, size_t len) {
    const auto* source = static_cast<const ExportSource*>(ctx);
    if (offset + len > source->bytes->size()) { return -1; }
    std::memcpy(out, source->bytes->data() + offset, len);
    return 0;
}

int test_random(void*, uint8_t* out, size_t len) {
    static uint8_t value = 0x33;
    for (size_t i = 0; i < len; ++i) { out[i] = value++; }
    return 0;
}

void write_backup_file(const std::string& path, const std::vector<uint8_t>& store,
                       const std::string& password) {
    ExportSource source{&store};
    FILE* file = std::fopen(path.c_str(), "wb");
    require(file != nullptr, "open backup file for writing");
    const int result = cf_backup_export(file, reinterpret_cast<const uint8_t*>(password.data()),
                                        password.size(), pico_serial_hash, source_read,
                                        &source, test_random, nullptr);
    require(std::fclose(file) == 0, "close written backup file");
    require(result == CF_BACKUP_OK, "export generated backup");
}

bool verify_file(const std::string& path, const std::string& password) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) { return false; }
    const int result = cf_backup_verify(file, reinterpret_cast<const uint8_t*>(password.data()),
                                        password.size(), pico_serial_hash);
    std::fclose(file);
    return result == CF_BACKUP_OK;
}

const char kFile[] = "FIDOBAK.CFB";
const char kPrevious[] = "FIDOBAK.OLD";
const char kTemporary[] = "FIDOBAK.TMP";
const std::string kPassword = "correcthorse12";
const std::string kOldPassword = "olderpassword";
const std::string kPrevPassword = "oldestpasswd";

/* ------------------------------------------------------------------ */
/* Scenarios                                                           */
/* ------------------------------------------------------------------ */

void scenario_pending_blocks_normal_boot() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    nvs_put("pending", 1);
    add_delete();
    add_notice_ok();
    add_stop("recovery-loop", "Storage recovery");
    const RunResult result = run_startup();
    require_stopped(result, "recovery-loop");
    require(nvs_read("mode") == static_cast<int>(BackupMode::Save),
            "pending guard must not consume the queued mode");
    require(nvs_read("pending") == 1, "pending stays set");
    require(nvs_erase_calls == 0, "NVS is never auto-erased");
    require(prepare_calls == 0, "no maintenance mode started before USB");
    require(partition_erases == 0 && partition_writes == 0,
            "guard blocks partition writes");
    require(restart_calls == 0, "guard blocks normal boot path");
    require(saw_title("Storage recovery") && saw_title("USB disabled"),
            "recovery is the only entry point");
}

void scenario_unreadable_pending_fails_closed() {
    nvs_put("mode", 0);
    nvs_put("pending", 1);
    nvs_fail_get_key = "pending";
    add_stop("recovery-loop", "Storage recovery");
    const RunResult result = run_startup();
    require_stopped(result, "recovery-loop");
    require(partition_erases == 0 && partition_writes == 0,
            "unreadable guard must not boot");
    require(nvs_erase_calls == 0, "NVS is never auto-erased");
    require(restart_calls == 0, "no normal boot after NVS error");
}

void scenario_nvs_init_failure_fails_closed() {
    nvs_fail_init = 1;
    add_stop("recovery-loop", "Storage recovery");
    const RunResult result = run_startup();
    require_stopped(result, "recovery-loop");
    require(partition_erases == 0 && partition_writes == 0,
            "NVS failure must not touch the store");
    require(nvs_erase_calls == 0, "NVS failure is not repaired by erasing");
    require(restart_calls == 0, "NVS failure keeps USB off");
}

void scenario_mode_clear_failure_fails_closed() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    nvs_put("pending", 0);
    nvs_fail_set_key = "mode";
    nvs_fail_set_value = 0;
    nvs_fail_set_times = 1;
    add_stop("recovery-loop", "Storage recovery");
    const RunResult result = run_startup();
    require_stopped(result, "recovery-loop");
    require(nvs_read("mode") == static_cast<int>(BackupMode::Save),
            "uncleared mode keeps the request pending");
    require(prepare_calls == 0, "mode is not started when it cannot be cleared");
    require(partition_erases == 0, "no destructive action on NVS error");
    require(restart_calls == 0, "no normal boot after NVS error");
}

void scenario_storage_prepare_failure() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    prepare_result = -1;
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Storage layout error"), "storage layout failure is surfaced");
    require(first_event("nvs.set:mode=0") >= 0 && first_event("prepare") >= 0 &&
            first_event("nvs.set:mode=0") < first_event("prepare"),
            "mode is cleared before storage is touched");
    require(spi_init_calls == 0 && sd_mount_calls == 0, "no SD access after failure");
    require(partition_erases == 0 && partition_writes == 0, "no partition writes");
    require(restart_calls == 1, "reboot after non-destructive failure");
}

void scenario_unhealthy_store_refuses_export() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes.assign(CF_FIDO_STORE_SIZE, 0xFF);
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Backup refused"), "unhealthy store is not exported");
    require(saw_text("Store is not healthy"), "reason is displayed");
    require(spi_init_calls == 0 && sd_mount_calls == 0, "SD is not touched");
    require(partition_writes == 0 && partition_erases == 0, "store is read only");
    require(restart_calls == 1, "reboot after refusal");
}

void scenario_mount_uses_cardputer_adv_pins() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    sd_mount_result = ESP_FAIL;
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(spi_init_calls == 1 && sd_mount_calls == 1 && spi_free_calls == 1,
            "failed mount frees the SPI bus");
    require(saw_title("SD unavailable"), "missing SD is surfaced");
    require(last_mount_base == "/sdcard", "mount point");
    require(last_spi_host == SPI2_HOST, "dedicated SPI2 host");
    require(last_bus_config.mosi_io_num == 14, "MOSI pin");
    require(last_bus_config.miso_io_num == 39, "MISO pin");
    require(last_bus_config.sclk_io_num == 40, "SCLK pin");
    require(last_bus_config.quadwp_io_num == -1 && last_bus_config.quadhd_io_num == -1,
            "no quad pins");
    require(last_bus_config.max_transfer_sz == static_cast<int>(CF_BACKUP_CHUNK + 16),
            "DMA transfer size covers one chunk");
    require(last_device_config.gpio_cs == 12, "SD chip select pin");
    require(last_device_config.host_id == SPI2_HOST, "SD device on SPI2");
    require(last_sd_host.slot == SPI2_HOST, "SDSPI host slot");
    require(last_sd_host.max_freq_khz == 10000, "SD clock limit");
    require(last_mount_config.format_if_mount_failed == false,
            "never format an unreadable card");
    require(last_mount_config.max_files == 3, "three open files");
    require(last_mount_config.allocation_unit_size == 4096, "cluster size");
    require(partition_writes == 0 && partition_erases == 0, "save never writes flash");
}

void scenario_spi_init_failure() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    spi_init_result = ESP_FAIL;
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(spi_init_calls == 1 && sd_mount_calls == 0 && spi_free_calls == 0,
            "failed bus init does not free or mount");
    require(saw_title("SD unavailable"), "bus failure is surfaced");
    require(restart_calls == 1, "reboot after bus failure");
}

void scenario_request_mode_validation() {
    require(!cardputer::request_backup_mode(BackupMode::None),
            "None is not a persisted request");
    require(nvs_read("mode") < 0, "None writes nothing");
    require(cardputer::request_backup_mode(BackupMode::Save), "Save request persists");
    require(nvs_read("mode") == static_cast<int>(BackupMode::Save), "Save mode value");
    require(cardputer::request_backup_mode(BackupMode::Restore), "Restore persists");
    require(nvs_read("mode") == static_cast<int>(BackupMode::Restore), "Restore value");
    nvs_fail_set_key = "mode";
    nvs_fail_set_value = static_cast<int>(BackupMode::Save);
    nvs_fail_set_times = 1;
    require(!cardputer::request_backup_mode(BackupMode::Save),
            "failed readback rejects the request");
    require(nvs_read("mode") == static_cast<int>(BackupMode::Restore),
            "failed request does not change the previous mode");
}

void require_only_files(std::initializer_list<const char*> expected) {
    std::vector<std::string> want;
    for (const char* name : expected) { want.emplace_back(name); }
    std::sort(want.begin(), want.end());
    const auto got = list_dir(test_root);
    require(got == want, "unexpected files on the SD card");
}

void scenario_save_success_first_generation() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    sd_mount_result = ESP_OK;
    add_password(kPassword);
    add_password(kPassword);
    add_enter();
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Backup verified"), "verified backup is reported");
    require_only_files({kFile});
    require(verify_file(path_of(kFile), kPassword), "latest backup authenticates");
    require(nvs_read("mode") == 0, "mode request consumed");
    require(nvs_read("pending") <= 0, "no restore guard for export");
    require(partition_writes == 0 && partition_erases == 0, "export is read only");
    require_sd_released();
    require(restart_calls == 1, "reboot after successful backup");
}

void scenario_save_rotates_previous() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kOldPassword);
    write_backup_file(path_of(kPrevious), make_store(3), kPrevPassword);
    const auto old_latest = file_bytes(path_of(kFile));
    add_password(kPassword);
    add_password(kPassword);
    add_enter();
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require_only_files({kFile, kPrevious});
    require(verify_file(path_of(kFile), kPassword), "new latest authenticates");
    require(verify_file(path_of(kPrevious), kOldPassword),
            "old latest becomes the previous generation");
    require(file_bytes(path_of(kPrevious)) == old_latest,
            "previous generation is byte-identical");
    require_sd_released();
    require(restart_calls == 1, "reboot after rotation");
}

void scenario_save_export_failure_preserves_generations() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kOldPassword);
    write_backup_file(path_of(kPrevious), make_store(3), kPrevPassword);
    const auto latest_before = file_bytes(path_of(kFile));
    const auto previous_before = file_bytes(path_of(kPrevious));
    add_password(kPassword);
    add_password(kPassword);
    add_enter([] { fail_read_call = partition_reads + 1; });
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Backup failed"), "export failure is reported");
    require(file_bytes(path_of(kFile)) == latest_before, "latest is preserved");
    require(file_bytes(path_of(kPrevious)) == previous_before, "previous is preserved");
    require(verify_file(path_of(kFile), kOldPassword), "preserved latest still verifies");
    require(file_exists(path_of(kTemporary)), "partial temp file is identifiable");
    require_sd_released();
}

void scenario_save_fsync_failure_preserves_generations() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kOldPassword);
    const auto latest_before = file_bytes(path_of(kFile));
    add_password(kPassword);
    add_password(kPassword);
    add_enter([] { fail_fsync = true; });
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Backup failed"), "sync failure is reported");
    require(file_bytes(path_of(kFile)) == latest_before, "latest is preserved");
    require(fsync_calls > 0, "the temporary file is synced");
    require(file_exists(path_of(kTemporary)), "unsynced temp file is retained");
    require_sd_released();
}

void scenario_save_verify_failure_preserves_generations() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kOldPassword);
    const auto latest_before = file_bytes(path_of(kFile));
    add_password(kPassword);
    add_password(kPassword);
    add_enter([] { corrupt_tmp_on_verify = true; });
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Backup failed"), "readback authentication failure is reported");
    require(file_bytes(path_of(kFile)) == latest_before, "latest is preserved");
    require(file_exists(path_of(kTemporary)), "unverified temp file is retained");
    require_sd_released();
}

void scenario_save_rename_latest_failure_preserves_latest() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kOldPassword);
    const auto latest_before = file_bytes(path_of(kFile));
    add_password(kPassword);
    add_password(kPassword);
    add_enter([] { fail_rename_from = path_of(kFile); });
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Backup failed"), "rename failure is reported");
    require(file_exists(path_of(kFile)), "latest remains in place");
    require(file_bytes(path_of(kFile)) == latest_before, "latest is unchanged");
    require(verify_file(path_of(kFile), kOldPassword), "latest still authenticates");
    require(file_exists(path_of(kTemporary)), "new file remains as temp");
    require_sd_released();
}

void scenario_save_rename_temp_failure_preserves_previous() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kOldPassword);
    const auto latest_before = file_bytes(path_of(kFile));
    add_password(kPassword);
    add_password(kPassword);
    add_enter([] { fail_rename_from = path_of(kTemporary); });
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Backup failed"), "rename failure is reported");
    require(!file_exists(path_of(kFile)), "latest was rotated away first");
    require(file_exists(path_of(kPrevious)), "previous generation exists");
    require(file_bytes(path_of(kPrevious)) == latest_before,
            "previous holds the old latest");
    require(verify_file(path_of(kTemporary), kPassword),
            "new backup survives as a verified temp file");
    require_sd_released();
}

void scenario_save_password_mismatch_changes_nothing() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    sd_mount_result = ESP_OK;
    add_password(kPassword);
    add_password("differentpass1");
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Passwords differ"), "mismatch is surfaced");
    require_only_files({});
    require(partition_writes == 0 && partition_erases == 0, "no flash access");
    require_sd_released();
    require(restart_calls == 1, "reboot after mismatch");
}

void scenario_restore_missing_file() {
    nvs_put("mode", static_cast<int>(BackupMode::Restore));
    sd_mount_result = ESP_OK;
    add_enter();
    add_password(kPassword);
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Restore failed"), "missing file is reported");
    require(partition_writes == 0 && partition_erases == 0, "no partition writes");
    require(nvs_read("pending") < 0, "no guard is set for a missing file");
    require_sd_released();
    require(restart_calls == 1, "reboot after non-destructive failure");
}

void scenario_restore_invalid_password() {
    nvs_put("mode", static_cast<int>(BackupMode::Restore));
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kOldPassword);
    const auto before = file_bytes(path_of(kFile));
    add_enter();
    add_password("wrongpassword");
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Backup rejected"), "bad password is rejected");
    require(!saw_title("Replace all keys?"), "confirmation is not shown before authentication");
    require(partition_writes == 0 && partition_erases == 0, "no partition writes");
    require(nvs_read("pending") < 0, "no restore guard is set");
    require(file_bytes(path_of(kFile)) == before, "backup file is unchanged");
    require_sd_released();
    require(restart_calls == 1, "reboot after rejected file");
}

void scenario_restore_corrupt_file() {
    nvs_put("mode", static_cast<int>(BackupMode::Restore));
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kPassword);
    auto corrupted = file_bytes(path_of(kFile));
    corrupted[CF_BACKUP_HEADER + 100] ^= 0x80;
    FILE* file = std::fopen(path_of(kFile).c_str(), "wb");
    require(file != nullptr, "rewrite corrupt backup");
    std::fwrite(corrupted.data(), 1, corrupted.size(), file);
    std::fclose(file);
    add_enter();
    add_password(kPassword);
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Backup rejected"), "corrupt file is rejected");
    require(partition_writes == 0 && partition_erases == 0, "no partition writes");
    require(nvs_read("pending") < 0, "no restore guard is set");
    require_sd_released();
    require(restart_calls == 1, "reboot after rejected file");
}

void scenario_restore_user_cancels() {
    nvs_put("mode", static_cast<int>(BackupMode::Restore));
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kPassword);
    add_enter();
    add_password(kPassword);
    add_delete();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Replace all keys?"), "confirmation is shown");
    require(partition_writes == 0 && partition_erases == 0, "cancel writes nothing");
    require(nvs_read("pending") < 0, "cancel does not set the guard");
    require_sd_released();
    require(restart_calls == 1, "reboot after cancel");
}

void scenario_restore_guard_write_failure() {
    nvs_put("mode", static_cast<int>(BackupMode::Restore));
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kPassword);
    nvs_fail_set_key = "pending";
    nvs_fail_set_value = 1;
    nvs_fail_set_times = 1;
    add_enter();
    add_password(kPassword);
    add_enter();
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Restore refused"), "unsettable guard blocks restore");
    require(partition_writes == 0 && partition_erases == 0,
            "guard failure precedes every destructive write");
    require(nvs_read("pending") < 0, "guard is not partially written");
    require_sd_released();
    require(restart_calls == 1, "reboot after refused restore");
}

void scenario_restore_write_failure_keeps_guard() {
    nvs_put("mode", static_cast<int>(BackupMode::Restore));
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kPassword);
    add_enter();
    add_password(kPassword);
    add_enter([] { fail_write_call = partition_writes + 1; });
    add_notice_ok();
    add_delete();
    add_notice_ok();
    add_stop("recovery-loop", "Storage recovery");
    const RunResult result = run_startup();
    require_stopped(result, "recovery-loop");
    require(saw_title("Restore incomplete"), "write failure is surfaced");
    require(nvs_read("pending") == 1, "guard survives a failed apply");
    require(nvs_read("mode") == 0, "mode request is consumed");
    require(restart_calls == 0, "failed apply never resumes normal boot");
    require(partition_erases > 0, "failure happened after destructive work started");
    require(saw_title("Storage recovery"), "next boot enters recovery");
    require(first_event("nvs.set:pending=1") >= 0 &&
            first_event("nvs.set:pending=1") < first_event_prefix("partition.erase:"),
            "guard is persisted before the first erase");
    require_sd_released();
}

void scenario_restore_finish_readback_failure_keeps_guard() {
    nvs_put("mode", static_cast<int>(BackupMode::Restore));
    sd_mount_result = ESP_OK;
    write_backup_file(path_of(kFile), make_store(2), kPassword);
    key_read_corrupt_after_erase0 = 2;
    add_enter();
    add_password(kPassword);
    add_enter();
    add_notice_ok();
    add_delete();
    add_notice_ok();
    add_stop("recovery-loop", "Storage recovery");
    const RunResult result = run_startup();
    require_stopped(result, "recovery-loop");
    require(saw_title("Restore incomplete"), "key readback failure is surfaced");
    require(nvs_read("pending") == 1, "guard is retained after readback mismatch");
    require(restart_calls == 0, "readback mismatch never boots normally");
    require_sd_released();
}

void scenario_restore_success_clears_guard_after_readback() {
    nvs_put("mode", static_cast<int>(BackupMode::Restore));
    sd_mount_result = ESP_OK;
    const auto restored = make_store(2);
    write_backup_file(path_of(kFile), restored, kPassword);
    store_bytes = make_store(1);
    add_enter();
    add_password(kPassword);
    add_enter();
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "successful restore reboots");
    require(saw_title("Keys restored"), "success is surfaced");
    require(store_bytes == restored, "whole store matches the backup");
    cf_device_keys keys = {};
    require(cf_key_record_decode(store_bytes.data(), &keys) == CF_KEY_READY,
            "restored key page is valid");
    require(nvs_read("pending") == 0, "guard is cleared after completion");
    require(nvs_read("mode") == 0, "mode request consumed");
    require(restart_calls == 1, "exactly one reboot");
    const long pending_set = first_event("nvs.set:pending=1");
    const long pending_clear = first_event("nvs.set:pending=0");
    const long first_erase = first_event_prefix("partition.erase:");
    require(pending_set >= 0 && first_erase >= 0 && pending_set < first_erase,
            "guard is set and committed before the first erase");
    require(last_key_read_event >= 0 && pending_clear >= 0 &&
            last_key_read_event < pending_clear,
            "guard clears only after the final key-page readback");
    require(first_event("restart") > pending_clear, "reboot happens after clearing");
    require_sd_released();
}

void scenario_password_final_char_with_enter() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    sd_mount_result = ESP_OK;
    add_release();
    add_typed("abcdefghijk");
    Step final;
    final.keys.any = final.keys.enter = true;
    final.typed = "l";
    add(std::move(final));
    add_password("abcdefghijkl");
    add_enter();
    add_notice_ok();
    const RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "maintenance boot reboots");
    require(saw_title("Backup verified"), "simultaneous final char and Enter are kept");
    require(verify_file(path_of(kFile), "abcdefghijkl"),
            "password includes the final simultaneous character");
    require_sd_released();
}

void scenario_retry_remounts_after_session() {
    nvs_put("mode", static_cast<int>(BackupMode::Save));
    store_bytes = make_store(1);
    sd_mount_result = ESP_OK;
    add_password(kPassword);
    add_password(kPassword);
    add_delete();
    RunResult result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "first attempt reboots");
    require_only_files({});
    require(spi_init_calls == 1 && sd_mount_calls == 1, "first attempt mounts once");
    require_sd_released();

    nvs_put("mode", static_cast<int>(BackupMode::Save));
    add_password(kPassword);
    add_password(kPassword);
    add_enter();
    add_notice_ok();
    result = run_startup();
    require(result.kind == RunResult::Kind::Restarted, "retry reboots");
    require(spi_init_calls == 2, "retry reinitializes the SPI bus");
    require(sd_mount_calls == 2, "retry mounts the possibly replaced card");
    require(sd_unmount_calls == 2 && spi_free_calls == 2,
            "each session releases its own mount");
    require(verify_file(path_of(kFile), kPassword), "retry writes a valid backup");
}

void scenario_unmount_failure_does_not_reuse_handle() {
    sd_unmount_result = ESP_FAIL;
    scenario_retry_remounts_after_session();
}

void scenario_recovery_enters_restore() {
    nvs_put("pending", 1);
    sd_mount_result = ESP_FAIL;
    add_enter();
    add_notice_ok();
    add_stop("recovery-loop", "Storage recovery");
    const RunResult result = run_startup();
    require_stopped(result, "recovery-loop");
    require(saw_title("Storage recovery"), "recovery is entered before USB");
    require(saw_title("SD unavailable"), "restore path runs from recovery");
    require(prepare_calls == 1 && sd_mount_calls == 1,
            "recovery offers only the restore workflow");
    require(partition_writes == 0 && partition_erases == 0, "no writes without a file");
    require(sd_unmount_calls == 0, "failed mount has no session to unmount");
    require(spi_free_calls == 1, "failed mount still releases the bus");
    require(restart_calls == 0, "recovery never returns to normal boot");
}

struct ScenarioEntry {
    const char* name;
    void (*run)();
};

const ScenarioEntry kScenarios[] = {
    {"pending_blocks_normal_boot", scenario_pending_blocks_normal_boot},
    {"unreadable_pending_fails_closed", scenario_unreadable_pending_fails_closed},
    {"nvs_init_failure_fails_closed", scenario_nvs_init_failure_fails_closed},
    {"mode_clear_failure_fails_closed", scenario_mode_clear_failure_fails_closed},
    {"storage_prepare_failure", scenario_storage_prepare_failure},
    {"unhealthy_store_refuses_export", scenario_unhealthy_store_refuses_export},
    {"mount_uses_cardputer_adv_pins", scenario_mount_uses_cardputer_adv_pins},
    {"spi_init_failure", scenario_spi_init_failure},
    {"request_mode_validation", scenario_request_mode_validation},
    {"save_success_first_generation", scenario_save_success_first_generation},
    {"save_rotates_previous", scenario_save_rotates_previous},
    {"save_export_failure_preserves_generations",
     scenario_save_export_failure_preserves_generations},
    {"save_fsync_failure_preserves_generations",
     scenario_save_fsync_failure_preserves_generations},
    {"save_verify_failure_preserves_generations",
     scenario_save_verify_failure_preserves_generations},
    {"save_rename_latest_failure_preserves_latest",
     scenario_save_rename_latest_failure_preserves_latest},
    {"save_rename_temp_failure_preserves_previous",
     scenario_save_rename_temp_failure_preserves_previous},
    {"save_password_mismatch_changes_nothing",
     scenario_save_password_mismatch_changes_nothing},
    {"restore_missing_file", scenario_restore_missing_file},
    {"restore_invalid_password", scenario_restore_invalid_password},
    {"restore_corrupt_file", scenario_restore_corrupt_file},
    {"restore_user_cancels", scenario_restore_user_cancels},
    {"restore_guard_write_failure", scenario_restore_guard_write_failure},
    {"restore_write_failure_keeps_guard", scenario_restore_write_failure_keeps_guard},
    {"restore_finish_readback_failure_keeps_guard",
     scenario_restore_finish_readback_failure_keeps_guard},
    {"restore_success_clears_guard_after_readback",
     scenario_restore_success_clears_guard_after_readback},
    {"password_final_char_with_enter", scenario_password_final_char_with_enter},
    {"retry_remounts_after_session", scenario_retry_remounts_after_session},
    {"unmount_failure_does_not_reuse_handle", scenario_unmount_failure_does_not_reuse_handle},
    {"recovery_enters_restore", scenario_recovery_enters_restore},
};

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const auto& entry : kScenarios) { std::printf("%s\n", entry.name); }
        return 0;
    }
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <scenario> <empty-directory>\n", argv[0]);
        return 2;
    }
    const std::string name = argv[1];
    test_root = argv[2];
    try {
        const ScenarioEntry* selected = nullptr;
        for (const auto& entry : kScenarios) {
            if (name == entry.name) { selected = &entry; }
        }
        if (!selected) {
            std::fprintf(stderr, "unknown scenario %s\n", name.c_str());
            return 2;
        }
        selected->run();
        std::printf("PASS %s\n", name.c_str());
        return 0;
    } catch (const Failure& failure) {
        std::printf("FAIL %s: %s\n", name.c_str(), failure.message.c_str());
        std::printf("last_screen=%s\n", last_title.c_str());
        for (const auto& item : events) { std::printf("event %s\n", item.c_str()); }
        return 1;
    } catch (const Restart&) {
        std::printf("FAIL %s: unexpected restart escape\n", name.c_str());
        return 1;
    } catch (const Stop& stop) {
        std::printf("FAIL %s: unexpected stop '%s'\n", name.c_str(), stop.tag.c_str());
        return 1;
    }
}
