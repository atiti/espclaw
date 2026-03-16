# ESPClaw Admin UI

## Purpose

The admin UI is the primary local control surface for the device. It exists to make setup and maintenance possible without a serial console once the board is provisioned.

## V1 Screens

### Overview

Shows:

- device profile
- provisioning mode
- SD workspace status
- selected provider
- enabled chat channel
- OTA status

This screen maps cleanly to a compact JSON response so the embedded backend can serve it without server-side templating.

Current route:

- `GET /api/status`

### Network

Allows:

- viewing current Wi-Fi state
- updating Wi-Fi credentials
- checking provisioning fallback mode

### Provider Auth

Allows:

- selecting `openai_codex`, `openai_compat`, or `anthropic_messages`
- editing base URL and model
- saving access tokens, refresh tokens, and account ids through protected endpoints
- importing Codex CLI credentials from `~/.codex/auth.json` in simulator mode

Secret tokens are stored in NVS and should be written through dedicated protected endpoints rather than dumped into editable JSON.

Current routes:

- `GET /api/auth/status`
- `PUT /api/auth/codex`
- `DELETE /api/auth/codex`
- `POST /api/auth/import-codex-cli`

Current UI behavior:

- `Save Auth` writes the current provider credentials to the auth store.
- `Import Codex CLI` imports ChatGPT/Codex credentials from the local Codex CLI auth file when running in the simulator.
- `Clear Auth` removes the stored credentials.
- The auth status panel intentionally shows metadata, not raw secrets.

### Channels

V1 exposes Telegram configuration:

- bot token
- polling interval
- allowlist
- media enablement

### Workspace

Allows editing the main user-facing control files:

- `AGENTS.md`
- `IDENTITY.md`
- `USER.md`
- `HEARTBEAT.md`
- `memory/MEMORY.md`

The backend should also expose file metadata for these control files so the UI can indicate whether the workspace has been bootstrapped correctly on the SD card.

Current route:

- `GET /api/workspace/files`

### Tools

Allows:

- inspecting the currently registered tool catalog
- seeing each tool name, summary, parameter schema, and safety level
- validating what the model can call without needing a chat roundtrip

Current route:

- `GET /api/tools`

Current UI behavior:

- `Refresh Tools` reloads the tool catalog JSON from the runtime.
- the chat loop can use `tool.list` when the user asks about available capabilities.

### Apps

Allows:

- listing installed Lua apps with id, version, permissions, and triggers
- loading an app manifest plus Lua source into an editor
- creating or updating an app bundle on SD
- running an app with a chosen trigger and payload body
- removing an installed app cleanly from `/workspace/apps/<app_id>/`

The backend contract stays JSON-first so the firmware can serve it from small handlers without templating or a database.

Current routes:

- `GET /api/apps`
- `GET /api/apps/detail?app_id=<id>`
- `POST /api/apps/scaffold?app_id=<id>`
- `PUT /api/apps/source?app_id=<id>`
- `POST /api/apps/run?app_id=<id>&trigger=<name>`
- `DELETE /api/apps?app_id=<id>`

Current UI behavior:

- `Create App` scaffolds a new Lua bundle and reloads the app list.
- Clicking an app card loads its manifest and `main.lua` into the editor.
- `Save Source` replaces the current entrypoint source with the textarea contents.
- `Run App` posts the payload textarea to the runtime and shows the returned result JSON.
- `Delete App` removes the app bundle from the workspace and clears the editor state.

### Control Loops

Allows:

- starting a persistent scheduled Lua loop by loop id, app id, trigger, payload, and period
- stopping a running loop without rebooting the device
- inspecting loop completion state, iteration counts, and the last app result

This is the current path for keeping a Lua VM warm for control-oriented apps such as self-balancing robots, rovers, and actuator test rigs.

Current routes:

- `GET /api/loops`
- `POST /api/loops/start?loop_id=<id>&app_id=<id>&trigger=<name>&period_ms=<n>&iterations=<n>`
- `POST /api/loops/stop?loop_id=<id>`

Current UI behavior:

- `Start Loop` launches a persistent Lua VM and begins calling `handle(trigger, payload)` on the requested schedule.
- `Stop Loop` sets `stop_requested` and lets the worker exit cleanly after the current iteration.
- `Refresh Loops` reloads the current loop inventory and status JSON.

### Chat

Allows:

- running an iterative LLM session against the configured provider
- inspecting the persisted session transcript for a chosen session id
- testing the same multi-tool loop that Telegram uses for free-text messages

Current routes:

- `POST /api/chat/run?session_id=<id>`
- `GET /api/chat/session?session_id=<id>`

Current UI behavior:

- `Run Chat` starts a single iterative run that may use multiple tool-call rounds before replying.
- `Load Session` reads the stored JSONL transcript for the selected session id and renders it for inspection.

### Logs And Diagnostics

Shows:

- agent loop status
- recent errors
- SD/NVS health
- memory budget hints based on the active board profile

### OTA

Allows:

- checking current version
- uploading a firmware image
- applying a pending image
- showing rollback state

## UX Constraints

- The UI must remain lightweight enough for embedded hosting.
- It should work on mobile browsers because ESP32 devices are often administered from phones.
- It should not require a JS framework runtime to be useful.
- Authentication is mandatory before config mutation routes are accepted.

## Host Simulator

The same HTML asset is served by the macOS/Linux host simulator, so UI work can be developed without hardware:

```bash
cmake -S . -B build
cmake --build build
./build/espclaw_simulator --workspace /tmp/espclaw-dev --port 8080 --profile esp32s3
```

Then visit `http://127.0.0.1:8080/`.
