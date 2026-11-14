"""Host verification for src/cardputer/backup.cpp maintenance orchestration.

The production backup.cpp is compiled byte-for-byte (with its real backup.h and
board.h) against narrow ESP-IDF shims for NVS, the fido_store partition, SPI/SD
mount and the board key/screen surface. Startup routing, the restore guard,
file-generation preservation and SD session cleanup run as the real functions.
Every scenario starts in a fresh process and directory, so backup.cpp's static
mount flag and the boot-scoped NVS state behave like one power cycle.

The pinned Mbed TLS primitives and three C backup sources are built with
optimization in the test directory. The orchestration translation units use
ASan/UBSan; the local crypto archive does not. The separate native CTest codec
and store targets instrument the production C sources and crypto library too.
"""
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
STUB = ROOT / "tests" / "cardputer" / "backup_stub"
MBEDTLS = ROOT / "pico-keys-sdk" / "third-party" / "mbedtls"
MBEDTLS_CONFIG = ROOT / "tests" / "cardputer" / "backup_mbedtls_config.h"

MBEDTLS_SOURCES = [
    "aes", "cipher", "cipher_wrap", "constant_time", "gcm", "md", "pkcs5",
    "platform_util", "sha256",
]
CORE_SOURCES = [
    ROOT / "src/cardputer/backup_codec.c",
    ROOT / "src/cardputer/backup_store.c",
    ROOT / "src/cardputer/key_store.c",
]

REQUIRED_SCENARIOS = {
    "pending_blocks_normal_boot",
    "unreadable_pending_fails_closed",
    "nvs_init_failure_fails_closed",
    "mode_clear_failure_fails_closed",
    "storage_prepare_failure",
    "unhealthy_store_refuses_export",
    "mount_uses_cardputer_adv_pins",
    "spi_init_failure",
    "request_mode_validation",
    "save_success_first_generation",
    "save_rotates_previous",
    "save_export_failure_preserves_generations",
    "save_fsync_failure_preserves_generations",
    "save_verify_failure_preserves_generations",
    "save_rename_latest_failure_preserves_latest",
    "save_rename_temp_failure_preserves_previous",
    "save_password_mismatch_changes_nothing",
    "restore_missing_file",
    "restore_invalid_password",
    "restore_corrupt_file",
    "restore_user_cancels",
    "restore_guard_write_failure",
    "restore_write_failure_keeps_guard",
    "restore_finish_readback_failure_keeps_guard",
    "restore_success_clears_guard_after_readback",
    "password_final_char_with_enter",
    "retry_remounts_after_session",
    "unmount_failure_does_not_reuse_handle",
    "recovery_enters_restore",
}


def run_checked(command, *, cwd=None, timeout=120):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                            timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(
            "command failed: {}\nstdout:\n{}\nstderr:\n{}".format(
                " ".join(map(str, command)), result.stdout, result.stderr))
    return result


class BackupRuntimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="cardputer-backup-runtime-")
        cls.dir = pathlib.Path(cls.work.name)
        cls.build = cls.dir / "build"
        cls.build.mkdir()
        cls.cc = shlex.split(os.environ.get("CC", "cc"))
        cls.cxx = shlex.split(os.environ.get("CXX", "c++"))

        # board.h is quoted-included from backup.h, so compile a current copy
        # next to a stub keyboard header. backup.cpp itself stays unmodified.
        for name in ("backup.cpp", "backup.h", "board.h"):
            shutil.copy2(ROOT / "src" / "cardputer" / name, cls.build / name)
        (cls.build / "keyboard").mkdir()
        shutil.copy2(STUB / "keyboard" / "Keyboard.h",
                     cls.build / "keyboard" / "Keyboard.h")

        config_define = '-DMBEDTLS_CONFIG_FILE="{}"'.format(MBEDTLS_CONFIG)
        common = [
            "-std=gnu11", "-O2", "-funsigned-char",
            config_define,
            "-I", str(MBEDTLS / "include"),
            "-I", str(ROOT / "src" / "cardputer"),
        ]
        objects = []
        for name in MBEDTLS_SOURCES:
            source = MBEDTLS / "library" / (name + ".c")
            obj = cls.build / (name + ".o")
            run_checked(cls.cc + common + ["-c", str(source), "-o", str(obj)])
            objects.append(obj)
        for source in CORE_SOURCES:
            obj = cls.build / (source.stem + ".o")
            run_checked(cls.cc + common + ["-c", str(source), "-o", str(obj)])
            objects.append(obj)
        cls.core = cls.build / "libcore_fast.a"
        run_checked(["ar", "rcs", str(cls.core)] + list(map(str, objects)))

        cxx_flags = [
            "-std=c++17", "-funsigned-char", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
            config_define,
            "-I", str(cls.build),
            "-I", str(STUB),
            "-I", str(ROOT / "src" / "cardputer"),
            "-I", str(MBEDTLS / "include"),
        ]
        probe = cls.build / "backup_runtime_probe.o"
        run_checked(cls.cxx + cxx_flags + [
            "-c", str(STUB / "backup_runtime_probe.cpp"), "-o", str(probe)])
        backup = cls.build / "backup.o"
        run_checked(cls.cxx + cxx_flags + [
            "-c", str(cls.build / "backup.cpp"), "-o", str(backup)])

        cls.binary = cls.build / "backup_runtime_probe"
        run_checked(cls.cxx + [
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
            str(probe), str(backup), str(cls.core),
            "-Wl,--wrap=fopen", "-Wl,--wrap=stat", "-Wl,--wrap=remove",
            "-Wl,--wrap=rename", "-Wl,--wrap=fsync",
            "-o", str(cls.binary),
        ])

        shutil.copy2(ROOT / "src/cardputer/input.cpp", cls.build / "input.cpp")
        cls.input_binary = cls.build / "input_typing_probe"
        run_checked(cls.cxx + cxx_flags + [
            str(cls.build / "input.cpp"), str(STUB / "input_typing_probe.cpp"),
            "-o", str(cls.input_binary)])

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def scenario_env(self):
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        return env

    def test_maintenance_scenarios(self):
        listing = subprocess.run(
            [str(self.binary), "--list"], capture_output=True, text=True,
            timeout=30, check=True)
        scenarios = listing.stdout.split()
        self.assertEqual(len(scenarios), len(set(scenarios)),
                         "scenario names must be unique")
        self.assertTrue(REQUIRED_SCENARIOS.issubset(set(scenarios)),
                        "required maintenance scenarios are missing: {}".format(
                            sorted(REQUIRED_SCENARIOS - set(scenarios))))
        self.assertGreaterEqual(len(scenarios), 27)
        for name in scenarios:
            with self.subTest(scenario=name):
                with tempfile.TemporaryDirectory(
                        prefix="sd-", dir=self.dir) as card:
                    result = subprocess.run(
                        [str(self.binary), name, card],
                        capture_output=True, text=True, timeout=120,
                        env=self.scenario_env())
                    self.assertEqual(
                        result.returncode, 0,
                        "{}\nstdout:\n{}\nstderr:\n{}".format(
                            name, result.stdout, result.stderr))

    def test_fresh_password_input(self):
        result = subprocess.run([str(self.input_binary)], capture_output=True,
                                text=True, timeout=30, env=self.scenario_env())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_target_headers_compile(self):
        """Compile the current backup.cpp against the real ESP-IDF headers.

        This proves the SD pin/mount and NVS calls still match the installed
        target SDK; it is a syntax-only cross-compile, not a link or flash.
        """
        database = ROOT / "build" / "cardputer-adv" / "compile_commands.json"
        if not database.exists():
            self.skipTest("target compile database is not available")
        entries = json.loads(database.read_text())
        entry = next((item for item in entries
                      if item["file"].endswith("src/cardputer/board.cpp")), None)
        if entry is None:
            self.skipTest("no cardputer translation unit in the compile database")
        tokens = shlex.split(entry["command"])
        compiler = pathlib.Path(tokens[0])
        if not compiler.exists():
            self.skipTest("the ESP-IDF cross compiler is not installed")
        arguments = []
        index = 1
        while index < len(tokens):
            token = tokens[index]
            if token == "-o":
                index += 2
                continue
            if token == "-c":
                index += 1
                continue
            if token in ("-MT", "-MF"):
                index += 2
                continue
            if token in ("-MD", "-MMD") or token.startswith("-MF"):
                index += 1
                continue
            if token == entry["file"]:
                index += 1
                continue
            arguments.append(token)
            index += 1
        command = [str(compiler)] + arguments + [
            "-fsyntax-only", str(ROOT / "src" / "cardputer" / "backup.cpp")]
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=120)
        self.assertEqual(result.returncode, 0,
                         result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
