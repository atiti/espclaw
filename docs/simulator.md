# ESPClaw Simulator

## Purpose

`espclaw_simulator` is the host-side development harness for ESPClaw. It runs the same core workspace bootstrap, Lua app runtime, admin JSON renderers, and admin UI asset on macOS so firmware development can proceed without an ESP32 board on the desk.

This is a host runtime for the firmware surfaces, not a hardware emulator. It does not emulate Wi‑Fi radios, BLE, the camera stack, SDMMC timing, or flash/bootloader behavior.

## Build

```bash
cmake -S . -B build
cmake --build build
```

The simulator binary is produced at:

```text
build/espclaw_simulator
```

## Usage

```bash
./build/espclaw_simulator --workspace /tmp/espclaw-dev --port 8080 --profile esp32s3
```

Supported flags:

- `--workspace PATH`
- `--port PORT`
- `--profile esp32s3|esp32cam`
- `--self-test`

`--self-test` bootstraps a temporary workspace, scaffolds a demo app, runs it, and exits with a non-zero status on failure. That path is also used by the host test suite.

## Exposed Surface

The simulator binds to `127.0.0.1` and exposes the same admin routes as the firmware:

- `GET /`
- `GET /api/status`
- `GET /api/auth/status`
- `PUT /api/auth/codex`
- `DELETE /api/auth/codex`
- `POST /api/auth/import-codex-cli`
- `GET /api/workspace/files`
- `GET /api/tools`
- `GET /api/hardware`
- `GET /api/apps`
- `GET /api/apps/detail?app_id=<id>`
- `POST /api/apps/scaffold?app_id=<id>`
- `PUT /api/apps/source?app_id=<id>`
- `POST /api/apps/run?app_id=<id>&trigger=<name>`
- `DELETE /api/apps?app_id=<id>`
- `POST /api/chat/run?session_id=<id>`
- `GET /api/chat/session?session_id=<id>`
- `GET /api/tasks`
- `POST /api/tasks/start?task_id=<id>&app_id=<id>&schedule=<periodic|event>&trigger=<name>&period_ms=<n>&iterations=<n>`
- `POST /api/tasks/stop?task_id=<id>`
- `POST /api/events/emit?name=<event>`
- `GET /api/behaviors`
- `POST /api/behaviors/register?behavior_id=<id>&app_id=<id>&schedule=<periodic|event>&trigger=<name>&period_ms=<n>&iterations=<n>&autostart=<0|1>`
- `POST /api/behaviors/start?behavior_id=<id>`
- `POST /api/behaviors/stop?behavior_id=<id>`
- `DELETE /api/behaviors?behavior_id=<id>`
- `GET /api/loops`
- `POST /api/loops/start?loop_id=<id>&app_id=<id>&trigger=<name>&period_ms=<n>&iterations=<n>`
- `POST /api/loops/stop?loop_id=<id>`

It also runs the same Lua hardware API with simulated state for:

- GPIO reads and writes
- PWM channel setup, duty tracking, and servo pulse state
- PPM frame output state
- raw ADC values and derived millivolt reads
- UART console IO on `UART0` through simulator `stdin`/`stdout`
- camera capture to a deterministic 1x1 JPEG fixture under `/workspace/media/`
- register-level I2C devices plus typed `TMP102` and `MPU6050` decoding
- PID, mixer, and attitude helper math
- hardware event watches that turn UART input and ADC threshold crossings into local task events

## Example Flow

```bash
./build/espclaw_simulator --workspace /tmp/espclaw-dev --port 8080 --profile esp32cam
curl -s http://127.0.0.1:8080/api/status
curl -s http://127.0.0.1:8080/api/tools
curl -s -X POST http://127.0.0.1:8080/api/auth/import-codex-cli
curl -s -X POST 'http://127.0.0.1:8080/api/apps/scaffold?app_id=demo'
curl -s -X POST 'http://127.0.0.1:8080/api/apps/run?app_id=demo&trigger=manual' --data 'ping'
curl -s -X POST 'http://127.0.0.1:8080/api/chat/run?session_id=demo' --data 'Inspect the device and list installed apps.'
curl -s -X POST 'http://127.0.0.1:8080/api/chat/run?session_id=vision' --data 'Capture a camera image and describe it.'
curl -s -X POST 'http://127.0.0.1:8080/api/tasks/start?task_id=demo_loop&app_id=demo&schedule=periodic&trigger=timer&period_ms=20&iterations=3' --data 'tick'
curl -s -X POST 'http://127.0.0.1:8080/api/tasks/start?task_id=sensor_watch&app_id=demo&schedule=event&trigger=manual&iterations=2'
curl -s -X POST 'http://127.0.0.1:8080/api/events/emit?name=manual' --data 'near'
curl -s -X POST 'http://127.0.0.1:8080/api/behaviors/register?behavior_id=patrol&app_id=demo&schedule=periodic&trigger=timer&period_ms=100&iterations=0&autostart=1' --data 'patrol'
curl -s -X POST 'http://127.0.0.1:8080/api/behaviors/start?behavior_id=patrol'
curl -s 'http://127.0.0.1:8080/api/behaviors'
```

Then open `http://127.0.0.1:8080/` to use the admin UI.

## UART Console Bridging

On the host simulator, `UART0` is connected to the simulator process console:

- `espclaw.uart.write(0, "...")` writes directly to the terminal running `espclaw_simulator`
- `espclaw.uart.read(0, n)` consumes bytes typed into that same terminal

Because a normal terminal is line-buffered, apps usually receive keyboard input after you press Enter. This is intended for interactive simulator bring-up, serial-style command testing, and development against UART-based peripherals or protocols without real hardware attached.

## Vision Loop

The simulator now supports the full camera-to-LLM handoff path used by the iterative agent loop:

1. `camera.capture` writes a deterministic JPEG to `/workspace/media/`
2. the tool result is stored in the session transcript as normal tool output
3. the next provider request includes that image as an `input_image` block for vision-capable models

This is currently the safest place to validate the multimodal run loop before adding board-specific camera drivers on real hardware.

## Development Notes

- The simulator uses the vendored Lua source already present under the ESP-IDF managed component directory.
- Host Lua execution is enabled by the `ESPCLAW_HOST_LUA` compile definition.
- The simulator is intended to keep the core runtime testable while hardware-specific services are added behind the same interfaces.
- `tests/simulator_api_test.sh` boots the simulator and verifies tool catalog rendering, hardware discovery, auth storage, iterative chat runs, app CRUD, persisted behavior lifecycle, periodic and event-driven task lifecycle, and clean shutdown behavior.
