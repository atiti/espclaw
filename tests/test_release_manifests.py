#!/usr/bin/env python3

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.generate_release_manifests import build_manifest, build_release_entry, load_flasher_args


class ReleaseManifestTests(unittest.TestCase):
    def test_build_manifest_esp32_release_urls(self) -> None:
        flasher_args = load_flasher_args(ROOT / "firmware" / "build-esp32" / "flasher_args.json")
        manifest = build_manifest("atiti/espclaw", "v0.1.0", "esp32", flasher_args)
        self.assertEqual(manifest["name"], "ESPClaw")
        self.assertEqual(manifest["version"], "v0.1.0")
        self.assertEqual(manifest["builds"][0]["chipFamily"], "ESP32")
        parts = manifest["builds"][0]["parts"]
        self.assertEqual(parts[0]["offset"], 0x1000)
        self.assertTrue(parts[0]["path"].endswith("/bootloader-esp32.bin"))
        self.assertTrue(any(part["path"].endswith("/espclaw_firmware-esp32.bin") for part in parts))

    def test_build_release_entry_names(self) -> None:
        entry = build_release_entry("v0.1.0", "esp32s3", {"extra_esptool_args": {"chip": "esp32s3"}})
        self.assertEqual(entry["chip_family"], "ESP32-S3")
        self.assertEqual(entry["bundle_asset"], "espclaw-esp32s3-v0.1.0.zip")
        self.assertEqual(entry["manifest_asset"], "esp-web-tools-manifest-esp32s3.json")

    def test_manifest_and_entry_are_json_serializable(self) -> None:
        flasher_args = load_flasher_args(ROOT / "firmware" / "build-esp32s3" / "flasher_args.json")
        manifest = build_manifest("atiti/espclaw", "v9.9.9", "esp32s3", flasher_args)
        entry = build_release_entry("v9.9.9", "esp32s3", flasher_args)
        with tempfile.TemporaryDirectory() as tmpdir:
            manifest_path = Path(tmpdir) / "manifest.json"
            entry_path = Path(tmpdir) / "entry.json"
            manifest_path.write_text(json.dumps(manifest, indent=2))
            entry_path.write_text(json.dumps(entry, indent=2))
            self.assertIn("ESP32-S3", manifest_path.read_text())
            self.assertIn("esp-web-tools-manifest-esp32s3.json", entry_path.read_text())

    def test_site_redirects_www_to_apex(self) -> None:
        html = (ROOT / "site" / "index.html").read_text(encoding="utf-8")
        self.assertIn('rel="canonical" href="https://espclaw.dev/"', html)
        self.assertIn('window.location.hostname === "www.espclaw.dev"', html)
        self.assertIn('window.location.replace(next);', html)


if __name__ == "__main__":
    unittest.main()
