#include "backup.h"
#include "backup_codec.h"
#include "backup_password.h"
#include "backup_store.h"
#include "flash_layout.h"
#include "key_store_esp32.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
extern "C" {
#include "serial.h"
}

namespace cardputer {
namespace {
constexpr char kMount[] = "/sdcard";
// FAT 8.3 names also work when long-filename support is disabled.
constexpr char kFile[] = "/sdcard/FIDOBAK.CFB";
constexpr char kPrevious[] = "/sdcard/FIDOBAK.OLD";
constexpr char kTemporary[] = "/sdcard/FIDOBAK.TMP";
constexpr char kNamespace[] = "fido_backup";
constexpr auto kHost = SPI2_HOST;
bool mounted = false;
sdmmc_card_t* mounted_card = nullptr;
bool sd_teardown_failed = false;

bool state_read(const char* key, std::uint8_t& out) {
    out = 0;
    if (nvs_flash_init() != ESP_OK) { return false; }
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) { return true; }
    if (result != ESP_OK) { return false; }
    result = nvs_get_u8(handle, key, &out);
    nvs_close(handle);
    return result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND;
}

bool state_write(const char* key, std::uint8_t value) {
    if (nvs_flash_init() != ESP_OK) { return false; }
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) { return false; }
    esp_err_t result = nvs_set_u8(handle, key, value);
    if (result == ESP_OK) { result = nvs_commit(handle); }
    nvs_close(handle);
    std::uint8_t check = 255;
    return result == ESP_OK && state_read(key, check) && check == value;
}

bool mount_sd() {
    if (mounted) { return true; }
    // Cardputer ADV official pinmap: CS12, MOSI14, CLK40, MISO39.
    // The LCD uses SPI3, so this dedicated SPI2 bus is not shared with it.
    spi_bus_config_t bus = {};
    bus.mosi_io_num = GPIO_NUM_14;
    bus.miso_io_num = GPIO_NUM_39;
    bus.sclk_io_num = GPIO_NUM_40;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = CF_BACKUP_CHUNK + 16;
    if (spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { return false; }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = kHost;
    host.max_freq_khz = 10000;
    sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
    device.host_id = kHost;
    device.gpio_cs = GPIO_NUM_12;
    esp_vfs_fat_sdmmc_mount_config_t config = {};
    config.format_if_mount_failed = false;
    config.max_files = 3;
    config.allocation_unit_size = 4096;
    if (esp_vfs_fat_sdspi_mount(kMount, &host, &device, &config, &mounted_card) != ESP_OK) {
        spi_bus_free(kHost);
        return false;
    }
    mounted = true;
    return true;
}

struct SDSession {
    ~SDSession() {
        if (!mounted) { return; }
        // IDF can free the card/context even when VFS unregistration fails.
        // Never reuse that handle. A failed teardown requires a clean boot
        // before another attempt; any restore guard remains persistent.
        sdmmc_card_t* card = mounted_card;
        mounted = false;
        mounted_card = nullptr;
        const bool unmounted = esp_vfs_fat_sdcard_unmount(kMount, card) == ESP_OK;
        const bool freed = spi_bus_free(kHost) == ESP_OK;
        sd_teardown_failed = sd_teardown_failed || !unmounted || !freed;
    }
};

void pause() {
    vTaskDelay(pdMS_TO_TICKS(10));
}

Keys fresh_keys(Board& board) {
    // Require a full release between screens, including modifier keys.
    do {
        board.poll_input();
        pause();
    } while (board.keys().any);
    for (;;) {
        board.poll_input();
        if (board.keys().any) { return board.keys(); }
        pause();
    }
}

void notice(Board& board, const char* title, const char* line) {
    Snapshot view;
    view.layout = Layout::Prompt;
    view.icon = Icon::Lock;
    view.title = title;
    std::snprintf(view.rows[0].text, sizeof(view.rows[0].text), "%s", line);
    view.row_count = 1;
    view.footer = "ENT:OK";
    board.render(view);
    while (!fresh_keys(board).enter) {}
}

bool confirm(Board& board, const char* title, const char* line) {
    Snapshot view;
    view.layout = Layout::Prompt;
    view.icon = Icon::Lock;
    view.title = title;
    std::snprintf(view.rows[0].text, sizeof(view.rows[0].text), "%s", line);
    view.row_count = 1;
    view.footer = "ENT:YES DEL:NO";
    board.render(view);
    for (;;) {
        const Keys keys = fresh_keys(board);
        if (keys.del) { return false; }
        if (keys.enter) { return true; }
    }
}

bool enter_password(Board& board, BackupPassword& password, const char* title,
                    std::size_t minimum) {
    password.clear();
    // Swallow the key used to enter this screen, including held printable keys.
    do { board.poll_input(); pause(); } while (board.keys().any);
    Keys previous;
    std::uint64_t last_activity = Board::now_ms();
    for (;;) {
        Snapshot view;
        view.layout = Layout::List;
        view.title = title;
        view.row_count = 3;
        const std::size_t stars = std::min<std::size_t>(password.size(), 16);
        std::memset(view.rows[0].text, '*', stars);
        std::snprintf(view.rows[1].text, sizeof(view.rows[1].text), "%u / 64 chars",
                      static_cast<unsigned>(password.size()));
        std::snprintf(view.rows[2].text, sizeof(view.rows[2].text), "Min %u characters",
                      static_cast<unsigned>(minimum));
        view.status = "FN+DEL:CANCEL";
        view.footer = "ENT:OK DEL:ERASE";
        board.render(view);
        board.poll_input();
        const Keys keys = board.keys();
        if (keys.any) { last_activity = Board::now_ms(); }
        if ((board.fn_held() && keys.del) || Board::now_ms() - last_activity > 120000) {
            password.clear();
            return false;
        }
        if (keys.del && !previous.del) {
            password.backspace();
        } else {
            password.append(board.typed());
            if (keys.enter && !previous.enter) {
                if (password.size() >= minimum) { return true; }
                board.play(Tone::Failure);
            }
        }
        previous = keys;
        pause();
    }
}

int read_store(void* ctx, std::uint32_t offset, std::uint8_t* out, std::size_t len) {
    return esp_partition_read(static_cast<const esp_partition_t*>(ctx), offset, out, len) == ESP_OK ? 0 : -1;
}
int program_store(void* ctx, std::uint32_t offset, const std::uint8_t* data, std::size_t len) {
    return esp_partition_write(static_cast<const esp_partition_t*>(ctx), offset, data, len) == ESP_OK ? 0 : -1;
}
int erase_store(void* ctx, std::uint32_t offset, std::size_t len) {
    return esp_partition_erase_range(static_cast<const esp_partition_t*>(ctx), offset, len) == ESP_OK ? 0 : -1;
}
int random_fill(void*, std::uint8_t* out, std::size_t len) {
    esp_fill_random(out, len);
    return 0;
}
int database_not_erased(void*) { return 0; }

bool verify_path(const char* path, const BackupPassword& password) {
    FILE* file = std::fopen(path, "rb");
    if (!file) { return false; }
    const int result = cf_backup_verify(file, password.data(), password.size(), pico_serial_hash);
    const int closed = std::fclose(file);
    return result == CF_BACKUP_OK && closed == 0;
}

void progress(Board& board, const char* title) {
    Snapshot view;
    view.layout = Layout::Prompt;
    view.icon = Icon::Lock;
    view.title = title;
    view.row_count = 1;
    std::snprintf(view.rows[0].text, sizeof(view.rows[0].text), "Keep power and SD on");
    view.footer = "PLEASE WAIT";
    board.render(view);
}

bool save_backup(const BackupPassword& password) {
    // Never replace a good file until the complete new file has been closed,
    // synced, reopened and authenticated. FAT is not power-loss atomic: retain
    // the previous generation and allow its explicit selection on restore.
    FILE* file = std::fopen(kTemporary, "wb");
    if (!file) { return false; }
    int result = cf_backup_export(file, password.data(), password.size(), pico_serial_hash,
                  read_store, const_cast<esp_partition_t*>(cardputer_fido_partition()), random_fill, nullptr);
    if (result == CF_BACKUP_OK && fsync(fileno(file)) != 0) { result = CF_BACKUP_IO; }
    if (std::fclose(file) != 0) { result = CF_BACKUP_IO; }
    if (result != CF_BACKUP_OK || !verify_path(kTemporary, password)) { return false; }
    struct stat info;
    if (stat(kFile, &info) == 0) {
        if (std::remove(kPrevious) != 0 && errno != ENOENT) { return false; }
        if (std::rename(kFile, kPrevious) != 0) { return false; }
    } else if (errno != ENOENT) {
        return false;
    }
    return std::rename(kTemporary, kFile) == 0 && verify_path(kFile, password);
}

const char* choose_file(Board& board) {
    int selected = 0;
    for (;;) {
        Snapshot view;
        view.title = "Restore from SD";
        view.row_count = 3;
        const char* names[] = {"Latest backup", "Previous backup", "Temporary file"};
        for (int i = 0; i < 3; ++i) {
            std::snprintf(view.rows[i].text, sizeof(view.rows[i].text), "%s", names[i]);
            view.rows[i].selected = selected == i;
        }
        view.footer = "ENT:OK DEL:BACK";
        board.render(view);
        const Keys keys = fresh_keys(board);
        if (keys.del) { return nullptr; }
        if (keys.up) { selected = (selected + 2) % 3; }
        else if (keys.down) { selected = (selected + 1) % 3; }
        else if (keys.enter) {
            const char* paths[] = {kFile, kPrevious, kTemporary};
            return paths[selected];
        }
    }
}

bool restore_backup(Board& board, const char* path, const BackupPassword& password) {
    FILE* file = std::fopen(path, "rb");
    if (!file) { notice(board, "Restore failed", "File not found"); return false; }
    progress(board, "Checking backup");
    int result = cf_backup_verify(file, password.data(), password.size(), pico_serial_hash);
    if (result != CF_BACKUP_OK) {
        std::fclose(file);
        notice(board, "Backup rejected", "Password/file/device");
        return false;
    }
    if (!confirm(board, "Replace all keys?", "Restores older state")) {
        std::fclose(file);
        return false;
    }
    auto* store = static_cast<cf_backup_store*>(std::calloc(1, sizeof(cf_backup_store)));
    if (!store) { std::fclose(file); return false; }
    store->ctx = const_cast<esp_partition_t*>(cardputer_fido_partition());
    store->read = read_store;
    store->program = program_store;
    store->erase = erase_store;
    // Persist AND read back before any destructive operation. It survives
    // partial key commits, including a power cut during the final key pages.
    if (!state_write("pending", 1)) {
        std::fclose(file);
        std::free(store);
        notice(board, "Restore refused", "Cannot set guard");
        return false;
    }
    progress(board, "Restoring keys");
    result = cf_backup_restore(file, password.data(), password.size(), pico_serial_hash,
                               cf_backup_store_write, store);
    if (std::fclose(file) != 0) { result = CF_BACKUP_IO; }
    if (result == CF_BACKUP_OK && cf_backup_store_finish(store) != 0) { result = CF_BACKUP_IO; }
    mbedtls_platform_zeroize(store, sizeof(*store));
    std::free(store);
    if (result == CF_BACKUP_OK && state_write("pending", 0)) {
        notice(board, "Keys restored", "Press Enter to reboot");
        esp_restart();
    }
    notice(board, "Restore incomplete", "Retry; USB stays off");
    return false;
}

void run_mode(Board& board, BackupMode mode) {
    if (cardputer_key_store_prepare() != 0) {
        notice(board, "Storage layout error", "See install guide");
        return;
    }
    // Export only a valid store; never provision a new identity in maintenance.
    if (mode == BackupMode::Save) {
        cf_key_flash flash = {};
        flash.ctx = const_cast<esp_partition_t*>(cardputer_fido_partition());
        flash.read = read_store;
        flash.program = program_store;
        flash.erase = erase_store;
        flash.database_erased = database_not_erased;
        cf_device_keys keys;
        const auto status = cf_key_store_load(&flash, &keys);
        mbedtls_platform_zeroize(&keys, sizeof(keys));
        if (status != CF_KEY_READY) {
            notice(board, "Backup refused", "Store is not healthy");
            return;
        }
    }
    if (!mount_sd()) {
        notice(board, "SD unavailable", "Insert FAT32 microSD");
        return;
    }
    SDSession session;
    const char* path = mode == BackupMode::Restore ? choose_file(board) : nullptr;
    if (mode == BackupMode::Restore && !path) { return; }
    BackupPassword password;
    if (!enter_password(board, password, "Backup password", mode == BackupMode::Save ? CF_BACKUP_PASSWORD_MIN : 1)) {
        return;
    }
    if (mode == BackupMode::Save) {
        BackupPassword repeated;
        if (!enter_password(board, repeated, "Repeat password", CF_BACKUP_PASSWORD_MIN)) { return; }
        if (!password.equals(repeated)) { notice(board, "Passwords differ", "Backup not changed"); return; }
        repeated.clear();
        if (!confirm(board, "Save backup to SD?", "Keep power and SD on")) { return; }
        progress(board, "Saving backup");
        const bool ok = save_backup(password);
        password.clear();
        notice(board, ok ? "Backup verified" : "Backup failed", ok ? "Remove SD after reboot" : "Keep previous backup");
    } else {
        restore_backup(board, path, password);
    }
}
}  // namespace

bool request_backup_mode(BackupMode mode) {
    return (mode == BackupMode::Save || mode == BackupMode::Restore) &&
           state_write("mode", static_cast<std::uint8_t>(mode));
}

void backup_startup(Board& board) {
    std::uint8_t mode = 0, pending = 0;
    if (!state_read("mode", mode) || !state_read("pending", pending)) {
        backup_recovery(board);
    }
    if (pending != 0) { backup_recovery(board); }
    if (mode == 0) { return; }
    if (!state_write("mode", 0)) { backup_recovery(board); }
    if (mode == static_cast<unsigned>(BackupMode::Save) || mode == static_cast<unsigned>(BackupMode::Restore)) {
        run_mode(board, static_cast<BackupMode>(mode));
    }
    if (sd_teardown_failed) { esp_restart(); }
    if (!state_read("pending", pending) || pending != 0) { backup_recovery(board); }
    esp_restart();
}

[[noreturn]] void backup_recovery(Board& board) {
    for (;;) {
        if (confirm(board, "Storage recovery", "Restore backup from SD?")) {
            run_mode(board, BackupMode::Restore);
            if (sd_teardown_failed) { esp_restart(); }
        } else {
            notice(board, "USB disabled", "Restore required");
        }
    }
}
}  // namespace cardputer
