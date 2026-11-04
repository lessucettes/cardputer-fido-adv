import hashlib
import importlib.util
import pathlib
import struct
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "cardputer" / "launcher-layout.py"


def load_planner():
    spec = importlib.util.spec_from_file_location("launcher_layout", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def make_entry(label, part_type, subtype, offset, size, flags=0):
    raw = struct.pack("<HBBII", 0xAA50, part_type, subtype, offset, size)
    raw += label.encode("ascii").ljust(16, b"\x00")
    raw += struct.pack("<I", flags)
    assert len(raw) == 32
    return raw


def make_table(entries):
    body = b"".join(entries)
    return body + struct.pack("<H", 0xEBEB) + b"\x00" * 30 + hashlib.md5(body).digest()


class LauncherLayoutTest(unittest.TestCase):
    def setUp(self):
        self.planner = load_planner()

    def entries(self, app_size=0x300000, flash=0x800000):
        return [
            {
                "label": "nvs", "type": 1, "subtype": 2,
                "offset": 0x9000, "size": 0x6000, "flags": 0,
            },
            {
                "label": "app0", "type": 0, "subtype": 0x10,
                "offset": 0x20000, "size": app_size, "flags": 0,
            },
        ]

    def test_creates_in_free_region(self):
        report = self.planner.plan_layout(self.entries(), 0x800000)
        self.assertEqual(report["action"], "create")
        self.assertEqual(report["offset"], 0x320000)
        self.assertEqual(report["size"], 0x102000)
        self.assertEqual(len(report["proposed"]), 3)

    def test_preserves_existing_offsets(self):
        report = self.planner.plan_layout(self.entries(), 0x800000)
        for original in report["existing"]:
            self.assertIn(original, report["proposed"])

    def test_no_space_refuses(self):
        with self.assertRaises(self.planner.LayoutError):
            self.planner.plan_layout(self.entries(app_size=0x3E0000), 0x400000)

    def test_fragmented_gap_falls_through(self):
        entries = [
            {"label": "a", "type": 0, "subtype": 0x10,
             "offset": 0x10000, "size": 0x1000, "flags": 0},
            {"label": "b", "type": 0, "subtype": 0x10,
             "offset": 0x30000, "size": 0x1000, "flags": 0},
        ]
        report = self.planner.plan_layout(entries, 0x800000)
        self.assertEqual(report["offset"], 0x31000)

    def test_duplicate_labels_refuse(self):
        entries = self.entries()
        entries.append(dict(entries[0]))
        with self.assertRaises(self.planner.LayoutError):
            self.planner.plan_layout(entries, 0x800000)

    def test_existing_valid_fido_is_kept(self):
        entries = self.entries()
        entries.append({
            "label": "fido_store", "type": 0x40, "subtype": 1,
            "offset": 0x320000, "size": 0x102000, "flags": 0,
        })
        report = self.planner.plan_layout(entries, 0x800000)
        self.assertEqual(report["action"], "keep")
        self.assertEqual(report["offset"], 0x320000)

    def test_existing_invalid_fido_is_reported(self):
        entries = self.entries()
        entries.append({
            "label": "fido_store", "type": 0x40, "subtype": 1,
            "offset": 0x320000, "size": 0x1000, "flags": 0,
        })
        report = self.planner.plan_layout(entries, 0x800000)
        self.assertEqual(report["action"], "invalid-existing")

    def test_overflowing_partition_refuses(self):
        entries = self.entries()
        entries.append({
            "label": "bad", "type": 0, "subtype": 0x10,
            "offset": 0x7FF000, "size": 0x2000, "flags": 0,
        })
        with self.assertRaises(self.planner.LayoutError):
            self.planner.plan_layout(entries, 0x800000)

    def test_parse_round_trip_and_bad_md5(self):
        entries = self.entries()
        raw = make_table([
            make_entry(item["label"], item["type"], item["subtype"],
                       item["offset"], item["size"], item["flags"])
            for item in entries
        ])
        parsed = self.planner.parse_table(raw)
        self.assertEqual([item["label"] for item in parsed], ["nvs", "app0"])

        corrupted = bytearray(raw)
        corrupted[-1] ^= 0xFF
        with self.assertRaises(self.planner.LayoutError):
            self.planner.parse_table(bytes(corrupted))

    def test_cli_is_read_only_and_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            table = pathlib.Path(tmp) / "table.bin"
            table.write_bytes(make_table([
                make_entry("nvs", 1, 2, 0x9000, 0x6000),
                make_entry("app0", 0, 0x10, 0x20000, 0x300000),
            ]))
            output = pathlib.Path(tmp) / "report.json"
            rc = self.planner.main(
                ["--table", str(table), "--flash-size", "0x800000",
                 "--json-out", str(output)]
            )
            self.assertEqual(rc, 0)
            self.assertTrue(output.is_file())
            self.assertIn("read_only", output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
