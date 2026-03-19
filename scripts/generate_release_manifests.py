#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


CHIP_FAMILIES = {
    "esp32": "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c3": "ESP32-C3",
    "esp32c6": "ESP32-C6",
    "esp32h2": "ESP32-H2",
}

ASSET_NAME_MAP = {
    "bootloader": "bootloader-{target}.bin",
    "app": "espclaw_firmware-{target}.bin",
    "partition-table": "partition-table-{target}.bin",
    "otadata": "ota_data_initial-{target}.bin",
}


def load_flasher_args(path: Path) -> dict:
    return json.loads(path.read_text())


def build_manifest(repo: str, tag: str, target: str, flasher_args: dict) -> dict:
    chip = flasher_args.get("extra_esptool_args", {}).get("chip", target)
    chip_family = CHIP_FAMILIES.get(chip, chip.upper())
    base_url = f"https://github.com/{repo}/releases/download/{tag}"
    parts = []
    for part_key, asset_template in ASSET_NAME_MAP.items():
        part = flasher_args.get(part_key)
        if not part:
            continue
        parts.append(
            {
                "path": f"{base_url}/{asset_template.format(target=target)}",
                "offset": int(part["offset"], 16),
            }
        )
    return {
        "name": "ESPClaw",
        "version": tag,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": chip_family,
                "parts": parts,
            }
        ],
    }


def build_release_entry(tag: str, target: str, flasher_args: dict) -> dict:
    chip = flasher_args.get("extra_esptool_args", {}).get("chip", target)
    chip_family = CHIP_FAMILIES.get(chip, chip.upper())
    return {
        "target": target,
        "chip_family": chip_family,
        "bundle_asset": f"espclaw-{target}-{tag}.zip",
        "manifest_asset": f"esp-web-tools-manifest-{target}.json",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate GitHub Release metadata for ESPClaw firmware artifacts.")
    parser.add_argument("--repo", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--flasher-args", required=True, type=Path)
    parser.add_argument("--manifest-output", required=True, type=Path)
    parser.add_argument("--entry-output", required=True, type=Path)
    args = parser.parse_args()

    flasher_args = load_flasher_args(args.flasher_args)
    manifest = build_manifest(args.repo, args.tag, args.target, flasher_args)
    release_entry = build_release_entry(args.tag, args.target, flasher_args)

    args.manifest_output.parent.mkdir(parents=True, exist_ok=True)
    args.entry_output.parent.mkdir(parents=True, exist_ok=True)
    args.manifest_output.write_text(json.dumps(manifest, indent=2) + "\n")
    args.entry_output.write_text(json.dumps(release_entry, indent=2) + "\n")


if __name__ == "__main__":
    main()
