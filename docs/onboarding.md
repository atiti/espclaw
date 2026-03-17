# ESPClaw Onboarding

## Provisioning

On first boot, ESPClaw picks the onboarding transport from the active board profile.

- `esp32s3` profile prefers BLE provisioning
- `esp32cam` profile uses ESPClaw-owned SoftAP onboarding
- if BLE is not enabled in the active ESP-IDF sdkconfig, `esp32s3` falls back to ESPClaw-owned SoftAP onboarding automatically

If credentials are already stored in Wi-Fi flash/NVS state, ESPClaw skips onboarding and starts station mode directly.

## BLE Flow

BLE onboarding is the primary zero-config path for the default `esp32s3` profile when Bluetooth is enabled in the firmware build.

Behavior:

- the device advertises a BLE provisioning service named `ESPClaw-xxxxxx`
- the default Proof-of-Possession is `espclaw-pass`
- `GET /api/network/provisioning` reports the service name, transport, PoP, QR payload, and the Espressif QR helper URL
- the firmware logs the same helper URL to the serial console during provisioning startup

The QR payload format matches Espressif's standard provisioning helper, so the admin UI, simulator, or a serial log can all hand the same onboarding data to a phone or desktop provisioning flow.

## Zero-Config SoftAP Flow

SoftAP onboarding is the default zero-serial path for `esp32cam` and the fallback path for BLE-disabled `esp32s3` builds.

Behavior:

- the device starts an AP named `ESPClaw-xxxxxx`
- the admin UI is immediately available on `http://192.168.4.1/`
- `GET /api/network/status` reports the `onboarding_ssid`, `provisioning_transport`, and `admin_url`
- `GET /api/network/provisioning` reports the active onboarding descriptor even on SoftAP boards
- the Network panel can scan nearby SSIDs and submit credentials without leaving the admin UI
- after the board gets a station IP, ESPClaw disables the onboarding AP and continues in STA mode

This avoids the socket contention that appears when trying to run the generic ESP-IDF SoftAP provisioning HTTP server and the ESPClaw admin server side by side on constrained boards.

## Workspace Storage

ESPClaw bootstraps the workspace layout on the SD card for both supported board profiles:

- SD card storage for `esp32s3`
- SD card storage for `esp32cam`

When workspace mounting succeeds, ESPClaw bootstraps:

- `AGENTS.md`
- `IDENTITY.md`
- `USER.md`
- `HEARTBEAT.md`
- `memory/MEMORY.md`

It also creates the `sessions/`, `media/`, `config/`, and `apps/` directories under the workspace root.

The `config/` directory now includes:

- `device.json` for runtime/provider settings
- `board.json` for board variants, named pins, and bus mappings

The board file can now be populated in three ways:

- leave the default `variant: "auto"` behavior
- apply a built-in preset from the admin UI
- upload or edit raw JSON directly through the board config API

## Telegram

Telegram polling is now configured at runtime from either:

- the admin UI `Advanced -> Channels` card
- the serial console via `/telegram ...` commands

Current behavior:

- `/status` returns a compact runtime status payload
- `/apps` lists installed Lua apps
- `/camera` or `/photo` captures a fresh JPEG on camera boards and uploads it into the Telegram chat
- `/newapp <app_id>` scaffolds a new Lua app in the workspace
- `/app <app_id> [payload]` runs an installed Lua app with the `telegram` trigger
- `/rmapp <app_id>` removes an installed Lua app from the workspace
- any other message is acknowledged and stored in the session transcript

This is a bring-up feature, not the final chatbot UX. It exists to validate connectivity, persistence, and outbound messaging on-device.
