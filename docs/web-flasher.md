# Web Flasher

ESPClaw ships a browser-based flasher at `https://espclaw.dev/`.

It is designed for the boring, high-value path:

- discover the latest GitHub release
- pick the right board family
- flash with Web Serial
- fall back to a downloadable bundle if browser flashing is unavailable

## How It Works

The flasher page is a static GitHub Pages site.

At runtime it:

1. queries the latest GitHub release from `atiti/espclaw`
2. finds the published `esp-web-tools` manifest for each target
3. renders browser flash buttons for:
   - `esp32`
   - `esp32s3`
4. links the corresponding release zip bundle for manual flashing

The release workflow publishes:

- `esp-web-tools-manifest-esp32.json`
- `esp-web-tools-manifest-esp32s3.json`
- `espclaw-esp32-<tag>.zip`
- `espclaw-esp32s3-<tag>.zip`

## Browser Requirements

Use a Chromium-class browser with Web Serial support:

- Chrome
- Edge
- Brave

Safari and Firefox are not sufficient for browser flashing here.

## First Flash

For most boards, the first wired flash gets ESPClaw onto the device.
After that, use the admin UI OTA path for most upgrades.

See [OTA](ota.md) for the current migration and partition-layout caveats.

## Manual Fallback

Every tagged release also publishes raw firmware assets and a zipped bundle per target, so users can still flash with `esptool.py` if browser flashing is unavailable or if their board needs a more manual bring-up path.
