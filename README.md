# ESPClaw

ESPClaw is an ESP32-native AI agent runtime inspired by OpenClaw, NanoClaw, and PicoClaw. It targets ESP32-S3-class boards first, while keeping classic ESP32-CAM support as a constrained compatibility profile.

The project is designed around a simple rule: remote LLMs, local tools. The device stores its workspace on SD, connects to Wi-Fi, exposes a local admin UI, talks to chat channels, and executes hardware-aware tool calls directly on the MCU.
It can also host Lua-based dynamic apps directly from the SD-backed workspace.
The current provider path now also supports OpenAI Codex-style ChatGPT account auth, so the iterative chat loop can run against ChatGPT/Codex subscription credentials instead of only API keys.

## Current State

This repository now contains the initial firmware scaffold and the host-testable core modules for:

- board profiles
- workspace bootstrap and file layout
- workspace filesystem bootstrap
- session JSONL persistence
- Lua app manifests, scaffolding, and SD-backed app discovery
- provider capability registry
- provider auth storage for API-key and ChatGPT/Codex-style backends
- provider request rendering
- iterative agent-loop orchestration with multi-tool turns and session continuation
- Telegram-first channel registry
- Telegram update parsing and reply payload rendering
- native hardware bindings for Lua apps across GPIO, PWM, servo/ESC pulse output, PPM, ADC, IMU, temperature sensors, buzzer, mixers, and PID loops
- simulator-backed UART console bridging on `UART0` via process `stdin`/`stdout`
- tool catalog and safety levels
- OTA state handling
- admin UI asset packaging
- admin API JSON rendering
- firmware-served admin HTTP routes for app management
- firmware-served control-loop HTTP routes for persistent scheduled Lua execution
- firmware- and simulator-served auth and chat routes for provider setup and session inspection
- default config rendering
- a host simulator for the firmware admin and Lua app runtime

The runtime integrations with ESP-IDF networking, Telegram polling, Lua app execution, camera capture, and OTA transport are intentionally staged behind these interfaces.
The firmware now also includes a live runtime path for NVS init, Wi‑Fi provisioning startup, SD-backed workspace bootstrap, Telegram polling/reply loops, Lua app boot hooks, provider auth storage, iterative LLM run execution, and the embedded admin HTTP server.

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

ESPClaw standardizes the SD-backed workspace layout:

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

The firmware now serves the admin UI directly from the device root path and exposes the app-management API on the same port:

- `GET /`
- `GET /api/status`
- `GET /api/auth/status`
- `PUT /api/auth/codex`
- `DELETE /api/auth/codex`
- `POST /api/auth/import-codex-cli`
- `GET /api/workspace/files`
- `GET /api/tools`
- `GET /api/apps`
- `GET /api/apps/detail?app_id=<id>`
- `POST /api/apps/scaffold?app_id=<id>`
- `PUT /api/apps/source?app_id=<id>`
- `POST /api/apps/run?app_id=<id>&trigger=<name>`
- `DELETE /api/apps?app_id=<id>`
- `POST /api/chat/run?session_id=<id>`
- `GET /api/chat/session?session_id=<id>`
- `GET /api/loops`
- `POST /api/loops/start?loop_id=<id>&app_id=<id>&trigger=<name>&period_ms=<n>&iterations=<n>`
- `POST /api/loops/stop?loop_id=<id>`

The firmware defaults now assume at least `4MB` flash because the Lua app runtime does not fit in the old `2MB` / `1MB app partition` layout.

Configure runtime options with `idf.py menuconfig` under `ESPClaw`, including:

- board profile
- SD mount point and SDSPI pins
- Telegram bot token
- Telegram polling interval

Provisioning transport is selected at runtime from the board profile, but BLE requires Bluetooth support in the active ESP-IDF sdkconfig. If BLE is not enabled, ESPClaw falls back to SoftAP provisioning and logs that downgrade during boot.

## Default Product Decisions

- Primary board profile: `esp32s3`
- Compatibility board profile: `esp32cam`
- Initial chat channel: Telegram
- Initial provider types: OpenAI-compatible, Anthropic Messages, and OpenAI Codex / ChatGPT OAuth
- Default admin surface: local web UI served by the device
- Default secret storage: NVS
- Default workspace storage: SD card
- Default dynamic app runtime: Lua

## Dynamic Apps

ESPClaw Lua apps live on SD under `/workspace/apps/<app_id>/`.

Each app contains:

- `app.json`: id, version, entrypoint, permissions, and triggers
- `main.lua`: the script entrypoint with a global `handle(trigger, payload)` function

Current runtime surfaces:

- boot-trigger apps run automatically after SD workspace mount
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

## Documentation

- Architecture: `docs/architecture.md`
- Admin UI: `docs/admin-ui.md`
- Apps: `docs/apps.md`
- Hardware Lua: `docs/hardware-lua.md`
- Vehicle Control: `docs/vehicle-control.md`
- Simulator: `docs/simulator.md`
