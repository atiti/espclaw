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
- `YOLO mode` adds a stronger system instruction for trusted local operator runs, telling the model to execute permitted tools directly instead of asking for another approval hop.
- local admin chat runs with mutation-enabled tools, so the model can install Lua apps, start tasks, and emit events from the browser.
- local admin chat can now also register and start persisted autonomous behaviors from the same tool loop.
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
- importing a pasted or uploaded `auth.json` payload directly from the browser
- importing Codex CLI credentials in simulator mode

Token textareas and raw auth import are tucked behind an `Advanced token fields and raw import` details panel so they do not dominate the normal setup view.

Current routes:

- `GET /api/auth/status`
- `PUT /api/auth/codex`
- `DELETE /api/auth/codex`
- `POST /api/auth/import-json`
- `POST /api/auth/import-codex-cli`

Current UI behavior:

- the default setup path is provider/model/account metadata plus a one-click auth import
- `Choose auth.json` lets the user load a local Codex/OpenClaw-style auth file into the browser and import it without copying individual fields
- `Import Pasted JSON` accepts the same payload pasted into the textarea
- credentials are stored in secure device auth storage and persist across normal reflashes unless NVS is erased

### Device

Allows:

- listing built-in board presets valid for the active board profile
- applying a built-in preset into `/workspace/config/board.json`
- editing the raw board descriptor JSON for custom carrier boards
- seeing the resolved board descriptor and current task-placement policy
- inspecting the live hardware capability map exposed to Lua and to the model tool loop
- checking workspace bootstrap state
- checking flash, RAM, CPU, and workspace usage through the system monitor
- checking camera support, initialization state, and the last capture result through a dedicated diagnostics panel
- running a direct test capture without going through the model loop

Current routes:

- `GET /api/board`
- `GET /api/hardware`
- `GET /api/board/presets`
- `GET /api/board/config`
- `PUT /api/board/config`
- `POST /api/board/apply?variant_id=<id>`
- `GET /api/camera`
- `POST /api/camera/capture?filename=<name>`

Current UI behavior:

- `Refresh` reloads the resolved descriptor.
- `Apply Preset` writes the chosen preset into the workspace and activates it immediately.
- the main board view is structured, while the raw `board.json` editor sits behind `Custom board descriptor`.
- `Refresh Camera` reloads the current camera diagnostics snapshot.
- `Test Capture` takes a fresh JPEG with the live hardware camera path and shows the saved workspace path or the real failure reason.
- when a capture succeeds, the panel links directly to the saved image through `/media/<path>`.

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
- seeing the active board memory class (`full` or `balanced`)
- seeing the current agent-loop budget for request/response buffers, instructions, tool-result slots, and history depth
- comparing the estimated agent working set with the recommended free-heap watermark for the active board profile

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
- `POST /api/apps/scaffold?app_id=<id>[&title=<title>&permissions=<csv>&triggers=<csv>]`
- `PUT /api/apps/source?app_id=<id>`
- `POST /api/apps/run?app_id=<id>&trigger=<name>`
- `DELETE /api/apps?app_id=<id>`

Current UI behavior:

- `Create App` scaffolds a new Lua bundle and reloads the app list.
- Clicking an app card loads its manifest and `main.lua` into the editor.
- `Save Source` replaces the current entrypoint source with the textarea contents.
- `Run App` posts the payload textarea to the runtime and shows the returned result JSON.
- `Delete App` removes the app bundle from the workspace and clears the editor state.

### Behaviors And Tasks

Allows:

- saving a persisted behavior definition that survives reboot
- marking a behavior `autostart` so it comes back after boot without calling the model
- starting or stopping a saved behavior without re-entering its config
- starting a persistent Lua task by task id, app id, schedule, trigger, payload, and period
- stopping a running task without rebooting the device
- emitting named events into event-driven tasks
- inspecting task completion state, event counts, iteration counts, and the last app result

This is the current path for keeping a Lua VM warm for control-oriented or reactive apps such as self-balancing robots, rovers, UART bridges, and sensor watchers.

Current routes:

- `GET /api/behaviors`
- `POST /api/behaviors/register?behavior_id=<id>&app_id=<id>&schedule=<periodic|event>&trigger=<name>&period_ms=<n>&iterations=<n>&autostart=<0|1>`
- `POST /api/behaviors/start?behavior_id=<id>`
- `POST /api/behaviors/stop?behavior_id=<id>`
- `DELETE /api/behaviors?behavior_id=<id>`
- `GET /api/tasks`
- `POST /api/tasks/start?task_id=<id>&app_id=<id>&schedule=<periodic|event>&trigger=<name>&period_ms=<n>&iterations=<n>`
- `POST /api/tasks/stop?task_id=<id>`
- `POST /api/events/emit?name=<event>`

Current UI behavior:

- `Save Behavior` persists an autonomous runtime definition to the workspace.
- `Autostart on boot` marks the behavior for automatic start after the device mounts storage and runs boot apps.
- `Start` and `Stop` act on the saved behavior definition instead of forcing the user to restate the whole task config.
- the behavior list shows whether the saved behavior is idle, running, or completed.
- `Start Task` launches a persistent Lua VM on either a periodic or event-driven schedule.
- `Emit` sends a named payload to event-driven tasks without restarting them.
- `Stop` sets `stop_requested` and lets the worker exit cleanly.
- `Refresh` reloads both the persisted behavior list and the live task inventory.

### Logs And Diagnostics

Shows:

- agent loop status
- recent errors
- SD/NVS health
- memory budget hints based on the active board profile

### OTA

The `Firmware OTA` card now performs a real firmware upload on device builds.

Flow:

1. Open the `Device` section.
2. Check the current OTA status.
3. Choose the built `espclaw_firmware.bin`.
4. Click `Upload and Reboot`.

The device writes the image to the inactive OTA slot, marks it as the next boot target, and schedules a reboot.

Existing boards still require one serial migration flash before this works, because the new OTA-capable partition table must be installed once.

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
