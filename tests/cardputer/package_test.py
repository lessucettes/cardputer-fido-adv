import hashlib
import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "cardputer" / "package.py"


def load_packager():
    spec = importlib.util.spec_from_file_location("package_cardputer", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def app_image() -> bytes:
    image = bytearray(256)
    image[0] = 0xE9
    image[1] = 0x03
    image[2] = 0x02
    image[12:14] = (9).to_bytes(2, "little")
    return bytes(image)


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self.build = self.root / "build"
        self.out = self.root / "out"
        self.build.mkdir()
        (self.build / "sdkconfig").write_text(
            'CONFIG_IDF_TARGET="esp32s3"\n'
            "CONFIG_TINYUSB_RHPORT_FS=y\n"
            "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y\n",
            encoding="utf-8",
        )
        (self.build / "pico_fido.elf").write_bytes(b"elf")
        (self.build / "pico_fido.bin").write_bytes(app_image())
        (self.build / "pico_fido.map").write_text(
            "key_store_esp32.c.o\nkey_store.c.o\nflash_layout.c.o\n",
            encoding="utf-8",
        )
        self.packager = load_packager()

    def tearDown(self):
        self.tmp.cleanup()

    def test_packages_only_app_manifest_and_instructions(self):
        manifest = self.packager.package(self.build, self.out)
        names = sorted(item.name for item in self.out.iterdir())
        self.assertEqual(
            names,
            ["Cardputer-FIDO-ADV.bin", "INSTALL.txt", "manifest.json"],
        )
        digest = hashlib.sha256((self.out / "Cardputer-FIDO-ADV.bin").read_bytes())
        self.assertEqual(manifest["firmware"]["sha256"], digest.hexdigest())
        loaded = json.loads((self.out / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(loaded["requires"]["partition"], "fido_store")

    def test_bootloader_in_build_is_not_packaged(self):
        (self.build / "bootloader.bin").write_bytes(b"boot")
        (self.build / "partition-table.bin").write_bytes(b"table")
        self.packager.package(self.build, self.out)
        names = {item.name for item in self.out.iterdir()}
        self.assertNotIn("bootloader.bin", names)
        self.assertNotIn("partition-table.bin", names)

    def test_invalid_app_image_refuses(self):
        (self.build / "pico_fido.bin").write_bytes(b"\x00" * 256)
        with self.assertRaises(ValueError):
            self.packager.package(self.build, self.out)

    def test_policy_failure_refuses(self):
        (self.build / "sdkconfig").write_text(
            'CONFIG_IDF_TARGET="esp32s3"\n'
            "CONFIG_TINYUSB_RHPORT_FS=y\n"
            "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y\n"
            "CONFIG_SECURE_BOOT=y\n",
            encoding="utf-8",
        )
        with self.assertRaises(ValueError):
            self.packager.package(self.build, self.out)

    def test_nonempty_output_refuses_without_force(self):
        self.out.mkdir()
        (self.out / "keep.txt").write_text("keep", encoding="utf-8")
        with self.assertRaises(ValueError):
            self.packager.package(self.build, self.out)
        self.packager.package(self.build, self.out, force=True)
        self.assertFalse((self.out / "keep.txt").exists())


if __name__ == "__main__":
    unittest.main()
