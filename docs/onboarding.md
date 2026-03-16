# ESPClaw Onboarding

## Provisioning

On first boot, ESPClaw starts the ESP-IDF provisioning manager.

- `esp32s3` profile prefers BLE provisioning
- `esp32cam` profile uses SoftAP provisioning
- `esp32c3` profile prefers BLE provisioning and defaults to internal flash storage
- if BLE is not enabled in the active ESP-IDF sdkconfig, the `esp32s3` profile falls back to SoftAP automatically
- when SoftAP provisioning is active, the ESPClaw admin UI is deferred until provisioning completes so it does not collide with the provisioning HTTP service on first boot

If credentials are already provisioned in NVS, ESPClaw skips provisioning mode and starts station mode directly.

## Workspace Storage

ESPClaw bootstraps the same workspace layout on either:

- SD card storage for `esp32s3` and `esp32cam`
- internal LittleFS flash storage for `esp32c3`

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

## Telegram

Telegram polling is enabled when a bot token is configured in `menuconfig`.

Current behavior:

- `/status` returns a compact runtime status payload
- `/apps` lists installed Lua apps
- `/newapp <app_id>` scaffolds a new Lua app in the workspace
- `/app <app_id> [payload]` runs an installed Lua app with the `telegram` trigger
- `/rmapp <app_id>` removes an installed Lua app from the workspace
- any other message is acknowledged and stored in the session transcript

This is a bring-up feature, not the final chatbot UX. It exists to validate connectivity, persistence, and outbound messaging on-device.
