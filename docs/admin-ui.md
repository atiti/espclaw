# ESPClaw Admin UI

## Purpose

The admin UI is the primary local control surface for the device. It exists to make setup and maintenance possible without a serial console, including first-boot onboarding on SoftAP-managed boards.

The current UI is chat-first rather than JSON-first:

- the default surface is an operator chat window with a transcript view and message composer
- setup work is grouped behind simpler modules for connectivity, device, apps, and advanced tools
- raw JSON is removed from the normal operator path and replaced by cards, chips, checklists, and metric tiles
- the custom `board.json` editor is still available, but only inside a collapsed advanced editor

The dashboard also lazy-loads by workflow. Only status and the chat surface load immediately; setup, apps, monitor, tools, and loop data load when the user opens the relevant module or tab.

## Operator Screens

### Chat

Shows:

- the persisted transcript for the selected session
- a ChatGPT-style compose box
- the final model reply for the latest run

Current routes:

- `POST /api/chat/run?session_id=<id>`
- `GET /api/chat/session?session_id=<id>`

Current UI behavior:

- `Send To Model` runs the iterative tool-using loop for the current session.
- `Refresh` reloads the stored transcript.
- the transcript is rendered as user / assistant / tool bubbles rather than raw JSONL.

### Overview

Shows:

- device profile
- provisioning mode
- workspace storage status
- selected provider
- enabled chat channel
- OTA status

This screen maps cleanly to a compact JSON response so the embedded backend can serve it without server-side templating.

Current route:

- `GET /api/status`

### Connectivity

Combines network onboarding and provider auth into one operator flow.

#### Network

Allows:

- viewing current Wi-Fi state
- scanning nearby networks
- updating Wi-Fi credentials
- seeing whether the device is still in onboarding mode
- seeing the active provisioning transport
- seeing the onboarding SSID and admin URL on SoftAP-backed boards
- seeing BLE service name, PoP, QR payload, and the Espressif QR helper URL on BLE-backed boards

Current routes:

- `GET /api/network/status`
- `GET /api/network/provisioning`
- `GET /api/network/scan`
- `POST /api/network/join`

Current UI behavior:

- `Scan Networks` queries nearby SSIDs and lets the user copy one into the SSID field.
- `Join Wi-Fi` submits credentials through the runtime and leaves the device in SoftAP onboarding until station join succeeds.
- the provisioning panel shows either the BLE onboarding descriptor or the SoftAP onboarding descriptor.
- on SoftAP onboarding builds, the status panel reports the transient AP name and `http://192.168.4.1/` admin URL.
- on BLE onboarding builds, the provisioning panel reports the service name, PoP, and helper URL that can be opened in Espressif's provisioning page.

#### Provider Auth

Allows:

- selecting the active model backend
- editing model, base URL, and account id
- saving or clearing tokens
- importing Codex CLI credentials in simulator mode

Token textareas are tucked behind a `Token fields` details panel so they do not dominate the normal setup view.

### Device

Allows:

- listing built-in board presets valid for the active board profile
- applying a built-in preset into `/workspace/config/board.json`
- editing the raw board descriptor JSON for custom carrier boards
- seeing the resolved board descriptor and current task-placement policy
- checking workspace bootstrap state
- checking flash, RAM, CPU, and workspace usage through the system monitor

Current routes:

- `GET /api/board`
- `GET /api/board/presets`
- `GET /api/board/config`
- `PUT /api/board/config`
- `POST /api/board/apply?variant_id=<id>`

Current UI behavior:

- `Refresh` reloads the resolved descriptor.
- `Apply Preset` writes the chosen preset into the workspace and activates it immediately.
- the main board view is structured, while the raw `board.json` editor sits behind `Custom board descriptor`.

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

### System Monitor

Allows:

- viewing CPU core count and dual-core status
- seeing CPU frequency and estimated per-core utilization
- checking flash capacity and current firmware image size
- checking workspace capacity and usage
- checking RAM total/free/minimum-free/largest-block values

Current route:

- `GET /api/monitor`

Current UI behavior:

- `Refresh Monitor` reloads the monitor snapshot from the runtime.
- the panel is meant as an operational health view for constrained boards, not a full profiler.

### Apps

Allows:

- listing installed Lua apps with id, version, permissions, and triggers
- loading an app manifest plus Lua source into an editor
- creating or updating an app bundle in the workspace
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
