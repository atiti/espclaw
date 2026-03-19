# Release Process

ESPClaw uses tagged GitHub releases for public artifacts.

## What A Release Produces

For each tag like `v0.2.0`, the release workflow builds and publishes:

- host simulator artifact
- `esp32` firmware bundle
- `esp32s3` firmware bundle
- `esp-web-tools` manifests for `esp32` and `esp32s3`
- checksums

Each firmware bundle should include:

- `espclaw_firmware.bin`
- `bootloader.bin`
- `partition-table.bin`
- `ota_data_initial.bin` when present
- `flasher_args.json`
- `esp-web-tools-manifest-<target>.json`
- `espclaw-<target>-<tag>.zip`
- `SHA256SUMS.txt`

The browser flasher at `espclaw.dev` consumes those published `esp-web-tools` manifests directly from the latest GitHub release.

## Preparing A Release

1. Update [CHANGELOG.md](../CHANGELOG.md).
2. Confirm docs reflect current operator and board behavior.
3. Run the local verification set:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
source scripts/use-idf.sh
cd firmware
idf.py -B build-esp32s3 set-target esp32s3
idf.py -B build-esp32s3 build
idf.py -B build-esp32 set-target esp32
idf.py -B build-esp32 build
```

4. Tag the release:

```bash
git tag v0.2.0
git push origin v0.2.0
```

## Release Notes Guidance

Each release should state:

- supported targets
- breaking changes or migration notes
- OTA safety notes
- major new runtime features
- known limitations

## OTA Guidance

If a release changes partition layout, storage layout, or boot/runtime recovery behavior, call it out explicitly in both the changelog and release notes.
