import importlib.util
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "cardputer" / "check-build.py"


def load_checker():
    spec = importlib.util.spec_from_file_location("check_build", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class BuildPolicyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.build = pathlib.Path(self.tmp.name)
        self.checker = load_checker()

    def tearDown(self):
        self.tmp.cleanup()

    def make_fixture(self, sdkconfig=None, map_text=None):
        if sdkconfig is None:
            sdkconfig = (
                'CONFIG_IDF_TARGET="esp32s3"\n'
                "CONFIG_TINYUSB_RHPORT_FS=y\n"
                "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y\n"
            )
        (self.build / "sdkconfig").write_text(sdkconfig, encoding="utf-8")
        (self.build / "pico_fido.elf").write_bytes(b"\x7fELF fixture")
        (self.build / "pico_fido.bin").write_bytes(b"bin fixture")
        if map_text is None:
            map_text = (
                "key_store_esp32.c.o\n"
                "key_store.c.o\n"
                "flash_layout.c.o\n"
            )
        (self.build / "pico_fido.map").write_text(map_text, encoding="utf-8")

    def test_valid_fixture_passes(self):
        self.make_fixture()
        self.assertEqual(self.checker.check_build(self.build), [])

    def test_missing_sdkconfig_fails(self):
        self.make_fixture()
        (self.build / "sdkconfig").unlink()
        self.assertTrue(self.checker.check_build(self.build))

    def test_secure_boot_fails(self):
        self.make_fixture(
            'CONFIG_IDF_TARGET="esp32s3"\n'
            "CONFIG_TINYUSB=y\n"
            "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y\n"
            "CONFIG_SECURE_BOOT=y\n"
        )
        self.assertTrue(self.checker.check_build(self.build))

    def test_flash_encryption_fails(self):
        self.make_fixture(
            'CONFIG_IDF_TARGET="esp32s3"\n'
            "CONFIG_TINYUSB=y\n"
            "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y\n"
            "CONFIG_SECURE_FLASH_ENC_ENABLED=y\n"
        )
        self.assertTrue(self.checker.check_build(self.build))

    def test_coredump_to_flash_fails(self):
        self.make_fixture(
            'CONFIG_IDF_TARGET="esp32s3"\n'
            "CONFIG_TINYUSB=y\n"
            "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y\n"
            "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y\n"
        )
        self.assertTrue(self.checker.check_build(self.build))

    def test_wrong_target_fails(self):
        self.make_fixture(
            'CONFIG_IDF_TARGET="esp32"\n'
            "CONFIG_TINYUSB=y\n"
            "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y\n"
        )
        self.assertTrue(self.checker.check_build(self.build))

    def test_original_otp_object_fails(self):
        self.make_fixture(map_text="otp_esp32.c.o\nkey_store.c.o\n")
        self.assertTrue(self.checker.check_build(self.build))

    def test_missing_replacement_object_fails(self):
        self.make_fixture(map_text="key_store.c.o\n")
        self.assertTrue(self.checker.check_build(self.build))

    def test_missing_binary_fails(self):
        self.make_fixture()
        (self.build / "pico_fido.bin").unlink()
        self.assertTrue(self.checker.check_build(self.build))


if __name__ == "__main__":
    unittest.main()
