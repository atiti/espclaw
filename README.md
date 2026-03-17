# ESPClaw

ESPClaw is an ESP32-native AI agent runtime inspired by OpenClaw, NanoClaw, and PicoClaw. It targets PSRAM-capable boards first, with ESP32-S3-class boards as the primary profile and classic AI Thinker ESP32-CAM as the camera-first compatibility profile.

The project is designed around a simple rule: remote LLMs, local tools. The device stores its workspace on SD, connects to Wi-Fi, exposes a local admin UI, talks to chat channels, and executes hardware-aware tool calls directly on the MCU.
It can also host Lua-based dynamic apps directly from the workspace filesystem.
The current provider path now also supports OpenAI Codex-style ChatGPT account auth, so the iterative chat loop can run against ChatGPT/Codex subscription credentials instead of only API keys.

## Current State

This repository now contains the initial firmware scaffold and the host-testable core modules for:

- board profiles
- workspace bootstrap and file layout
- workspace filesystem bootstrap
- session JSONL persistence
- Lua app manifests, scaffolding, and workspace-backed app discovery
- provider capability registry
- provider auth storage for API-key and ChatGPT/Codex-style backends
- provider request rendering
- iterative agent-loop orchestration with multi-tool turns and session continuation
- Telegram-first channel registry
- Telegram update parsing and reply payload rendering
- native hardware bindings for Lua apps across GPIO, PWM, servo/ESC pulse output, PPM, ADC, IMU, temperature sensors, buzzer, mixers, and PID loops
- simulator-backed UART console bridging on `UART0` via process `stdin`/`stdout`
- event-driven background tasks with periodic and named-event schedules
- hardware event watches that convert UART input and ADC threshold crossings into local named events
- persisted autonomous behaviors layered on top of the task runtime, with boot autostart support
- Lua trigger dispatch via `on_<trigger>(payload)`, `on_event(trigger, payload)`, or legacy `handle(trigger, payload)`
- dynamic hardware capability discovery through board-aware `hardware.list`
- simulator-backed camera capture plus image handoff into vision-capable model follow-up rounds
- tool catalog and safety levels
- real HTTP firmware OTA upload into dual OTA slots with reboot scheduling
- camera diagnostics and direct test-capture actions in the admin UI/API
- direct media serving for captured workspace JPEGs under `/media/...`
- admin UI asset packaging
- admin API JSON rendering
- firmware-served admin HTTP routes for app management
- firmware-served control-loop HTTP routes for persistent scheduled Lua execution
- firmware- and simulator-served auth and chat routes for provider setup and session inspection
- default config rendering
- a host simulator for the firmware admin and Lua app runtime

The firmware now also includes a live runtime path for NVS init, Wi‑Fi provisioning startup, SD workspace bootstrap, Telegram polling/reply loops, Lua app boot hooks, provider auth storage, iterative LLM run execution, direct HTTP OTA upload, and the embedded admin HTTP server.

## Repository Layout

```text
.
├── docs/
│   ├── admin-ui.md
│   └── architecture.md
├── firmware/
│   ├── CMakeLists.txt
│   ├── main/
│   │   ├── CMakeLists.txt
│   │   └── main.c
│   └── components/
│       └── espclaw_core/
├── tests/
├── CHANGELOG.md
└── CMakeLists.txt
```

## Workspace Layout

ESPClaw standardizes the workspace layout on the device SD card:

```text
/workspace/
├── AGENTS.md
├── IDENTITY.md
├── USER.md
├── HEARTBEAT.md
├── memory/
│   └── MEMORY.md
├── sessions/
├── media/
├── apps/
└── config/
    ├── board.json
    └── device.json
```

## Build And Test

The host-side tests do not require `idf.py`.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The host build also produces a local simulator binary:

```bash
./build/espclaw_simulator --workspace /tmp/espclaw-dev --port 8080 --profile esp32s3
```

Then open `http://127.0.0.1:8080/` to use the same admin UI and app-management API without hardware.

The simulator can import ChatGPT/Codex credentials from a local Codex CLI auth store:

```bash
curl -s -X POST http://127.0.0.1:8080/api/auth/import-codex-cli
```

That reads `CODEX_HOME/auth.json` or `~/.codex/auth.json` and stores the imported credentials into ESPClaw's auth store for subsequent chat runs.

You can also paste or upload a raw `auth.json` payload through the admin UI, or post it directly:

```bash
curl -s -X POST http://127.0.0.1:8080/api/auth/import-json \
  -H 'Content-Type: application/json' \
  --data '{"access_token":"...","refresh_token":"...","account_id":"acc_..."}'
```

Credentials are stored in device auth storage (`NVS` on firmware, workspace config in the simulator) and survive normal reflashes unless that storage is explicitly erased.

The local admin chat and simulator chat run with mutation-enabled tool access so the model can install Lua apps, start tasks, and emit events from the operator console. Telegram and other remote channels still keep confirmation gating for mutating tools.
The system prompt now also includes an automatic live tool inventory snapshot on every run, so the model starts with a readable capability list before it decides whether it needs a separate `tool.list` call.
Lua and app-generation runs also get a compact Lua contract snapshot sourced from the runtime API registry, and the model can call `lua_api.list` for the authoritative `espclaw.*` function surface when generating or debugging Lua code.

## Shared Console

ESPClaw now uses one shared console executor across:

- `UART0` on real hardware
- the simulator process console
- the admin web chat

Lines starting with `/` are treated as static commands. Everything else is passed through a normal iterative LLM turn.

Supported slash commands:

- `/help`
- `/status`
- `/tools`
- `/tool <name> [json]`
- `/wifi status`
- `/wifi scan`
- `/wifi join <ssid> [password]`
- `/memory`
- `/reboot`
- `/factory-reset`

Use `/tool` for direct operator execution of a specific tool:

```text
/tool system.info {}
/tool fs.read {"path":"USER.md"}
/tool web.search {"query":"ms5611 datasheet"}
```

The web UI chat now uses this same shared console path instead of a separate chat-only controller.

## Workspace Memory Files

The workspace bootstrap creates:

- `AGENTS.md`
- `IDENTITY.md`
- `USER.md`
- `HEARTBEAT.md`
- `memory/MEMORY.md`

Today these markdown files are injected directly into the system prompt whenever workspace storage is ready. They are not yet selectively retrieved, summarized, or indexed semantically.

They can be updated through normal workspace writes:

- `fs.write`
- `/tool fs.write {...}`
- `espclaw.fs.write(...)`

Keep them short, because they currently consume prompt budget on every run.

## Web Search And Fetch

ESPClaw now exposes two proxy-backed web tools:

- `web.search`
- `web.fetch`

They currently use the configured `llmproxy.markster.io` adapter to:

- run structured web searches
- fetch and scrape pages or documents
- persist larger fetched markdown into the workspace under `memory/web_fetch_<hash>.md`

These tools are available to the model and also through the shared operator console via `/tool`.

## Local ESP-IDF Setup

ESPClaw now supports a repo-local ESP-IDF installation so it does not depend on a global `~/.espressif` setup.

Bootstrap the toolchain:

```bash
mkdir -p .deps
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git .deps/esp-idf-v5.5.2
IDF_TOOLS_PATH=$PWD/.deps/.espressif ./.deps/esp-idf-v5.5.2/install.sh esp32s3,esp32
```

Activate it in a shell:

```bash
source scripts/use-idf.sh
```

Then build the firmware:

```bash
cd firmware
idf.py -B build-esp32s3 set-target esp32s3
idf.py -B build-esp32s3 build
idf.py -B build-esp32 set-target esp32
idf.py -B build-esp32 build
```

The firmware now serves the admin UI directly from the device root path and exposes board, network, app, auth, and chat APIs on the same port:

- `GET /`
- `GET /api/status`
- `GET /api/board`
- `GET /api/board/presets`
- `GET /api/board/config`
- `PUT /api/board/config`
- `POST /api/board/apply?variant_id=<id>`
- `GET /api/network/status`
- `GET /api/network/scan`
- `POST /api/network/join`
- `GET /api/auth/status`
- `PUT /api/auth/codex`
- `DELETE /api/auth/codex`
- `POST /api/auth/import-json`
- `POST /api/auth/import-codex-cli`
- `GET /api/workspace/files`
- `GET /api/monitor`
- `GET /api/tools`
- `GET /api/lua-api`
- `GET /api/lua-api.md`
- `GET /api/hardware`
- `GET /api/apps`
- `GET /api/apps/detail?app_id=<id>`
- `POST /api/apps/scaffold?app_id=<id>[&title=<title>&permissions=<csv>&triggers=<csv>]`
- `PUT /api/apps/source?app_id=<id>`
- `POST /api/apps/run?app_id=<id>&trigger=<name>`
- `DELETE /api/apps?app_id=<id>`
- `GET /api/behaviors`
- `POST /api/behaviors/register?behavior_id=<id>&app_id=<id>&schedule=<periodic|event>&trigger=<name>&period_ms=<n>&iterations=<n>&autostart=<0|1>`
- `POST /api/behaviors/start?behavior_id=<id>`
- `POST /api/behaviors/stop?behavior_id=<id>`
- `DELETE /api/behaviors?behavior_id=<id>`
- `POST /api/chat/run?session_id=<id>`
- `GET /api/chat/session?session_id=<id>`
- `GET /api/tasks`
- `POST /api/tasks/start?task_id=<id>&app_id=<id>&schedule=<periodic|event>&trigger=<name>&period_ms=<n>&iterations=<n>`
- `POST /api/tasks/stop?task_id=<id>`
- `POST /api/events/emit?name=<event>`
- `GET /api/loops`
- `POST /api/loops/start?loop_id=<id>&app_id=<id>&trigger=<name>&period_ms=<n>&iterations=<n>`
- `POST /api/loops/stop?loop_id=<id>`

The firmware defaults now assume at least `4MB` flash because the Lua app runtime does not fit in the old `2MB` / `1MB app partition` layout.

On the `esp32cam` profile, the internal flash `workspace` partition is only a fallback because the primary workspace lives on the SD card. The partition table therefore prioritizes two equally large `0x1E0000` OTA app slots and keeps only a small `0x030000` fallback internal workspace reserve so OTA remains viable as the firmware grows.

The vision/tool path is now end-to-end in the simulator, and `esp32cam` now has a real AI Thinker JPEG capture path. `camera.capture` writes a JPEG into `/workspace/media/`, and the next model round receives it as a Codex-compatible `input_image.image_url` attachment when the selected model supports vision.

For local operator testing, the admin chat now has a `YOLO mode` toggle. When enabled, the model is told to execute permitted tools directly instead of asking for an extra confirmation step. This is intended for bench and bring-up workflows on a trusted local surface.

Configure runtime options with `idf.py menuconfig` under `ESPClaw`, including:

- board profile
- storage backend override
- SD mount point and SDSPI pins
- flash workspace mount point and partition label
- Telegram bot token
- Telegram polling interval

Provisioning transport is selected at runtime from the board profile. `esp32s3` uses BLE-first onboarding, while `esp32cam` uses SoftAP onboarding.

For BLE-first boards such as the default `esp32s3` build:

- the runtime advertises a BLE provisioning service named `ESPClaw-xxxxxx`
- the default Proof-of-Possession is `espclaw-pass`
- `GET /api/network/provisioning` exposes the BLE service name, PoP, QR payload, and the Espressif QR helper URL
- the same helper URL is printed to the serial log during first-boot provisioning so boards can be onboarded without digging through mobile-app internals

For zero-serial onboarding on `esp32cam` and BLE-disabled `esp32s3` builds:

- the device brings up an onboarding AP named `ESPClaw-xxxxxx`
- the admin UI is available immediately on `http://192.168.4.1/`
- `GET /api/network/status` reports `onboarding_ssid`, `provisioning_transport`, and `admin_url`
- `GET /api/network/provisioning` reports the active provisioning descriptor for either BLE or SoftAP onboarding
- joining Wi-Fi through the admin UI or `POST /api/network/join` moves the device to STA mode and retires the onboarding AP after a successful connection

## System Monitor

ESPClaw exposes a lightweight runtime monitor through the admin UI and `GET /api/monitor`.

It reports:

- CPU core count and dual-core status
- CPU frequency and estimated per-core load
- flash chip size, active app partition size, and current app image size
- workspace storage total and used bytes
- RAM total, free, minimum-free, and largest-free-block values
- board-profile memory class such as `full` or `balanced`
- the active agent-loop budget, including request/response buffers, instruction budget, tool-result slot size, and history depth
- a target free-heap watermark so supported boards can be tuned against a visible operating budget

On simulator builds the values are synthetic but shape-compatible. On real firmware they come from ESP-IDF heap, flash, OTA, and idle-hook telemetry.

## Default Product Decisions

- Primary board profile: `esp32s3`
- Compatibility board profile: `esp32cam`
- Initial chat channel: Telegram
- Initial provider types: OpenAI-compatible, Anthropic Messages, and OpenAI Codex / ChatGPT OAuth
- Default admin surface: local web UI served by the device
- Default secret storage: NVS
- Default workspace storage: `sdcard`
- Default dynamic app runtime: Lua

On AI Thinker `esp32cam`, SD-backed workspace startup now tries the socket in conservative `sdmmc-1bit` mode first and then falls back to `sdspi` on the same socket pins (`CLK=14`, `CMD/MOSI=15`, `D0/MISO=2`, `D3/CS=13`) so storage bring-up is more tolerant of board/card variance.

## Board Configuration

ESPClaw now separates board class from board wiring.

- board profiles still define platform constraints such as camera support, storage backend, provisioning transport, and core count
- `config/board.json` defines the active board variant, named pin aliases, and default buses
- Lua apps can resolve named hardware resources through `espclaw.board.*` instead of hard-coding raw GPIO numbers
- Lua apps can share reusable modules through `/workspace/lib` and `/workspace/apps/<app_id>/lib` using normal `require(...)` calls
- Lua apps can inspect the live board and firmware surface through `espclaw.hardware.list()`

Built-in variants currently include:

- `generic_esp32s3`
- `ai_thinker_esp32cam`

The admin UI can:

- list built-in presets for the active profile
- apply a preset directly into `/workspace/config/board.json`
- edit and save the raw board descriptor JSON for custom wiring

Example:

```json
{
  "variant": "ai_thinker_esp32cam",
  "pins": {
    "servo_main": 4,
    "ppm_out": 5
  },
  "i2c": {
    "default": {
      "port": 0,
      "sda": 6,
      "scl": 7,
      "frequency_hz": 400000
    }
  },
  "adc": {
    "battery": {
      "unit": 1,
      "channel": 3
    }
  }
}
```

See `docs/board-config.md` for the full format and Lua usage.

## Task Placement

On dual-core targets like `esp32s3` and `esp32cam`, it now applies a simple task policy:

- admin HTTP server on core `0`
- Telegram polling on core `0`
- persistent control loops on core `1`

That keeps UI and network work separate from tight loop execution without hard-coding per-board task logic all over the runtime.

## Tasks And Events

ESPClaw now treats long-running Lua behavior as named tasks instead of only fixed control loops.

- periodic tasks keep a Lua VM alive and call a trigger such as `timer` on a fixed cadence
- event tasks keep a Lua VM alive and wait for named events such as `sensor`, `uart`, or any custom trigger you emit
- the runtime dispatches triggers in this order:
  - `on_<trigger>(payload)`
  - `on_event(trigger, payload)`
  - `handle(trigger, payload)`

The same task inventory is available to the admin API and to the model tool loop:

- `task.list`
- `task.start`
- `task.stop`
- `event.emit`

This is the intended substrate for persistent rover, robot, and embedded-agent behaviors that need to react to UART traffic, sensor conditions, timers, or operator commands without rebuilding firmware.

## Behaviors

Tasks are the live runtime workers. Behaviors are the persisted autonomous definitions that survive reboot and can autostart.

- a behavior saves `app_id`, `schedule`, `trigger`, `payload`, cadence, and autostart policy under `/workspace/behaviors`
- starting a behavior creates the matching live task using the behavior id as the task id
- stopping a behavior stops that live task without deleting the saved definition
- removing a behavior deletes the persisted definition and requests stop if it is currently running

The model can now use first-class behavior tools:

- `behavior.list`
- `behavior.register`
- `behavior.start`
- `behavior.stop`
- `behavior.remove`

`behavior.register` also accepts optional Lua source, so the model can install an app and persist the autonomous behavior that should run it in a single tool round.

## Dynamic Apps

ESPClaw Lua apps live under `/workspace/apps/<app_id>/` on either SD or internal flash.

Each app contains:

- `app.json`: id, version, entrypoint, permissions, and triggers
- `main.lua`: the script entrypoint with a global `handle(trigger, payload)` function

Current runtime surfaces:

- boot-trigger apps run automatically after workspace mount
- Telegram `/apps` lists installed apps with versions
- Telegram `/newapp <app_id>` scaffolds a new Lua app bundle
- Telegram `/app <app_id> [payload]` runs an installed app with the `telegram` trigger
- Telegram `/rmapp <app_id>` removes an installed app bundle
- The admin UI and HTTP API now expose live app inventory, detail, source update, run, scaffold, and delete operations
- Persistent control loops can now keep a Lua VM warm and call `handle(trigger, payload)` on a fixed period for balancing, drive, and actuator control tasks
- Lua apps can now call native hardware bindings for GPIO, PWM, servo/ESC pulses, PPM frames, ADC, I2C peripherals, typed IMU and temperature sensors, buzzer output, vehicle mixers, and PID control helpers
- On the host simulator, Lua apps can also use `espclaw.uart.read/write` with `UART0` bridged to the terminal running `espclaw_simulator`

## Chat Loop

ESPClaw now has a shared iterative run loop modeled after PicoClaw/OpenClaw's Responses-style orchestration:

- it loads `AGENTS.md`, `IDENTITY.md`, `USER.md`, `HEARTBEAT.md`, and `memory/MEMORY.md`
- it reads recent session history from `/workspace/sessions/<session_id>.jsonl`
- it sends tool schemas to the model
- it executes multiple tool-call rounds inside a single run
- it continues with `previous_response_id` until the model stops requesting tools or the iteration cap is hit
- it persists assistant and tool messages back to the session transcript

Current implemented tool execution inside the agent loop:

- `fs.read`
- `fs.list`
- `system.info`
- `wifi.status`
- `uart.read`
- `uart.write`
- `app.list`
- `app.run`

Mutating tools are confirmation-gated. When the model requests a mutating tool during a non-confirmed run, ESPClaw returns `confirmation_required` into the tool output and expects the model to ask the user before retrying.

The run loop is available from:

- Telegram free-text messages
- `POST /api/chat/run?session_id=<id>`
- the embedded admin UI Chat panel

Tool introspection is available from:

- `tool.list` in model tool-calling runs
- `GET /api/tools`
- model-driven chat runs where the assistant decides to call `tool.list`

Current provider auth options:

- `openai_codex`: ChatGPT/Codex-style account auth with `access_token`, `refresh_token`, and `account_id`
- `openai_compat`: API-key style bearer auth against an OpenAI-compatible `/responses` backend
- `anthropic_messages`: reserved in config and registry, with request rendering already present

These bindings are intended to make real embedded control applications possible:

- autonomous rover control with differential drive mixing and sensor feedback
- self-balancing robot attitude estimation with an MPU6050 and native PID loops
- quadcopter output generation with normalized quad-X mixer math and servo/ESC pulse control

## Simulator

`espclaw_simulator` is the host development entrypoint for the firmware runtime. It boots the same core workspace, app runtime, admin JSON renderers, and admin UI asset on macOS without ESP32 hardware.

Supported simulator surfaces:

- SD-like workspace bootstrap rooted at a local directory
- boot-trigger Lua apps
- provider auth import/save/clear flows
- iterative chat runs with persisted session transcripts
- admin UI at `/`
- tool catalog JSON at `/api/tools`
- the same app-management HTTP routes used by the firmware
- the same auth and chat routes used by the firmware
- the same control-loop HTTP routes used by the firmware
- `--self-test` mode for CI and local smoke testing

Example:

```bash
./build/espclaw_simulator --workspace /tmp/espclaw-dev --port 8080 --profile esp32cam
curl -s http://127.0.0.1:8080/api/status
curl -s http://127.0.0.1:8080/api/tools
curl -s -X POST http://127.0.0.1:8080/api/auth/import-codex-cli
curl -s -X POST 'http://127.0.0.1:8080/api/apps/scaffold?app_id=demo'
curl -s -X POST 'http://127.0.0.1:8080/api/apps/run?app_id=demo&trigger=manual' --data 'ping'
curl -s -X POST 'http://127.0.0.1:8080/api/chat/run?session_id=demo' --data 'Inspect the current workspace and list installed apps.'
curl -s -X POST 'http://127.0.0.1:8080/api/loops/start?loop_id=demo_loop&app_id=demo&trigger=manual&period_ms=20&iterations=5' --data 'tick'
```

The simulator is intentionally a host runtime, not a cycle-accurate hardware emulator. It does not emulate radio stacks, camera peripherals, or flash behavior yet.
It does simulate the Lua hardware API with in-memory GPIO, PWM, PPM, ADC, IMU/temperature sensor register state, and I2C state so control apps can be developed on macOS before flashing boards.

## Real Device Bench

Use the host-side bench to validate a live board progressively through the same admin API the UI uses:

```bash
python3 scripts/real_device_bench.py --device http://192.168.1.253:8080
```

The bench starts with a simple exact-text prompt and then moves toward tool use, LLM-generated Lua, and hardware-backed validation. Full details are in `docs/real-hardware-bench.md`.

The current default bench order is:

- `preflight`
- `inventory`
- `hello`
- `tool_reasoning`
- `generate_echo_app`
- `task_event_runtime`

## Documentation

- Architecture: `docs/architecture.md`
- Admin UI: `docs/admin-ui.md`
- Apps: `docs/apps.md`
- Console Chat: `docs/console-chat.md`
- Lua API: `docs/lua-api.md`
- Hardware Lua: `docs/hardware-lua.md`
- Real Hardware Bench: `docs/real-hardware-bench.md`
- Next Phase Plan: `docs/next-phase-plan.md`
- Web Tools: `docs/web-tools.md`
- Workspace Memory: `docs/workspace-memory.md`
- Vehicle Control: `docs/vehicle-control.md`
- Simulator: `docs/simulator.md`
