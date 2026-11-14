"""Exercise the real pre-maintenance handoff with recording RTOS/USB shims.

A failed flush must not restart and discard dirty SDK RAM. This covers the
handoff worker, not USB electrical detach or actual FreeRTOS scheduling.
"""
import os
import pathlib
import shlex
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PREAMBLE = r"""
#include <cassert>
#include <cstdint>
#include <string>
static std::uint64_t clock_ms;
static std::uint64_t busy_until;
static bool commit_ok, mode_ok, backup_failed;
static std::string events;
enum class BackupMode { Save, Restore };
static BackupMode requested_backup = BackupMode::Save;
struct Board { static std::uint64_t now_ms() { return clock_ms; } };
#define pdMS_TO_TICKS(n) (n)
static void vTaskDelay(unsigned ms) { clock_ms += ms; }
static bool is_busy() { return clock_ms < busy_until; }
static void tud_disconnect() { events += 'D'; }
static void tud_connect() { events += 'C'; }
static bool flash_commit_sync(unsigned ms) {
    assert(ms == 5000); events += 'F'; return commit_ok;
}
static bool request_backup_mode(BackupMode mode) {
    assert(mode == requested_backup); events += 'M'; return mode_ok;
}
static void vTaskDelete(void*) { events += 'X'; }
struct Restart {};
[[noreturn]] static void esp_restart() { events += 'R'; throw Restart{}; }
"""
CHECKS = r"""
int main() {
    for (int scenario = 0; scenario < 5; ++scenario) {
        clock_ms = 0; busy_until = 0; backup_failed = false;
        commit_ok = mode_ok = true; events.clear();
        if (scenario == 1) { busy_until = UINT64_MAX; }
        if (scenario == 2) { commit_ok = false; }
        if (scenario == 3) { mode_ok = false; }
        if (scenario == 4) { busy_until = 1000; }
        bool restarted = false;
        try { backup_reboot_task(nullptr); } catch (Restart&) { restarted = true; }
        if (scenario == 0 || scenario == 4) {
            assert(restarted && !backup_failed && events == "DFMR");
            assert(scenario != 4 || clock_ms >= busy_until);
        } else {
            assert(!restarted && backup_failed);
            if (scenario == 1) { assert(events == "DCX"); }
            if (scenario == 2) { assert(events == "DFCX"); }
            if (scenario == 3) { assert(events == "DFMCX"); }
        }
    }
}
"""


class BackupHandoffTest(unittest.TestCase):
    def test_flush_and_mode_failures_keep_running_state(self):
        source = (ROOT / "src/cardputer/ui.cpp").read_text()
        worker = source[source.index("void backup_reboot_task(void*)"):
                        source.index("void power_off_task(void*)")]
        with tempfile.TemporaryDirectory(prefix="backup-handoff-") as directory:
            cpp = pathlib.Path(directory) / "handoff.cpp"
            binary = pathlib.Path(directory) / "handoff"
            cpp.write_text(PREAMBLE + worker + CHECKS)
            built = subprocess.run(
                shlex.split(os.environ.get("CXX", "c++")) +
                ["-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 str(cpp), "-o", str(binary)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            tested = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
