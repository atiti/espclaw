# ESPClaw Onboarding

## Provisioning

On first boot, ESPClaw starts the ESP-IDF provisioning manager.

- `esp32s3` profile prefers BLE provisioning
- `esp32cam` profile uses SoftAP provisioning
- if BLE is not enabled in the active ESP-IDF sdkconfig, the `esp32s3` profile falls back to SoftAP automatically

If credentials are already provisioned in NVS, ESPClaw skips provisioning mode and starts station mode directly.

## SD Workspace

When SD mounting succeeds, ESPClaw bootstraps:

- `AGENTS.md`
- `IDENTITY.md`
- `USER.md`
- `HEARTBEAT.md`
- `memory/MEMORY.md`

It also creates the `sessions/`, `media/`, `config/`, and `apps/` directories under the workspace root.

## Telegram

Telegram polling is enabled when a bot token is configured in `menuconfig`.

Current behavior:

- `/status` returns a compact runtime status payload
- `/apps` lists installed Lua apps
- `/newapp <app_id>` scaffolds a new Lua app on the SD workspace
- `/app <app_id> [payload]` runs an installed Lua app with the `telegram` trigger
- `/rmapp <app_id>` removes an installed Lua app from the SD workspace
- any other message is acknowledged and stored in the session transcript

This is a bring-up feature, not the final chatbot UX. It exists to validate connectivity, persistence, and outbound messaging on-device.
