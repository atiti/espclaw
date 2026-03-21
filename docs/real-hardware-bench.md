# Real Hardware Bench

`scripts/real_device_bench.py` runs progressive checks against a real ESPClaw device over the admin HTTP API.

The default stage order is:

1. `preflight`
2. `inventory`
3. `hello`
4. `tool_reasoning`
5. `generate_echo_app`
6. `task_event_runtime`
7. `vision`

The intent is to move from simple reachability toward live tool inventory, model reasoning, persistent Lua behavior generation, local autonomous task execution, and real camera-to-LLM validation.

Additional stress and audit stages are available:

- `operator_surfaces`
  - exercises the exact operator-facing turn paths for `web`, `uart`, and `telegram`
  - uses the new `/api/bench/operator-turn` harness endpoint so surface routing can be tested without a serial cable or live Telegram delivery timing
  - records per-surface memory telemetry (`free_internal` and `largest_internal` before/after the turn) to catch allocation regressions on constrained boards
- `semantic_blink_task`
  - asks the live model for a concrete hardware behavior: build a blink task with explicit pin/count/timing and run it
  - validates the stored app source and task state instead of trusting the assistant prose
  - is intended to catch exactly the “installed a hello app instead of the requested hardware logic” failure mode
- `tool_matrix_full`
  - runs many smaller, audited prompts instead of one giant sweep
  - checks which tool names the model actually requested in the transcript
  - is the preferred way to measure real tool-call coverage on hardware
  - now exercises the broader executor surface one case at a time, including:
    - `tool.list`, `system.info`, `hardware.list`
    - `wifi.status`, `wifi.scan`, `ble.scan`
    - `fs.list`, `fs.write`, `fs.read`, `fs.delete`
    - app / behavior / task / event-watch lifecycle tools
    - `ota.check`, `camera.capture`
    - `gpio.read/write`, `pwm.write`, `ppm.write`
    - `adc.read`, `uart.read/write`
    - `i2c.scan/read/write`, `temperature.read`, `imu.read`
    - `buzzer.play`, `pid.compute`, `control.mix`, `spi.transfer`
- `large_lua_app`
  - asks the live model to build progressively larger Lua apps through `app.install`
  - verifies the installed source size and the app's runtime behavior
  - currently proves a real `2581` byte model-generated Lua app install/run on the AI Thinker `esp32cam`
  - the next failure still occurs in model-side `app.install` emission rather than at a proven device RAM ceiling

## Usage

Run against the default LAN device:

```bash
python3 scripts/real_device_bench.py
```

Run against a specific device:

```bash
python3 scripts/real_device_bench.py --device http://192.168.1.253:8080
```

Continue after failures and save a report:

```bash
python3 scripts/real_device_bench.py \
  --device http://192.168.1.253:8080 \
  --continue-on-failure \
  --output-json /tmp/espclaw-bench.json
```

Run only the early stages:

```bash
python3 scripts/real_device_bench.py --stages preflight,hello,tool_reasoning

# Run the explicit per-tool matrix with YOLO mode enabled
python3 scripts/real_device_bench.py \
  --stages operator_surfaces,semantic_blink_task \
  --session-prefix bench_surfaces \
  --yolo \
  --continue-on-failure

# Run the explicit per-tool matrix with YOLO mode enabled
python3 scripts/real_device_bench.py \
  --stages tool_matrix_full \
  --session-prefix bench_matrix \
  --yolo \
  --continue-on-failure

# Probe the largest LLM-generated Lua app that can really be installed and run
python3 scripts/real_device_bench.py \
  --stages large_lua_app \
  --session-prefix bench_large \
  --yolo \
  --continue-on-failure
```

## What Each Stage Verifies

- `preflight`
  - device is reachable
  - workspace storage is ready
  - provider auth is configured
- `inventory`
  - `/api/tools` exposes the core model tool surface
  - `/api/hardware` exposes the current board descriptor
  - `/api/workspace/files` confirms the control files exist on the live workspace
- `hello`
  - the live LLM loop returns the exact marker `ESPCLAW_BENCH_HI`
- `tool_reasoning`
  - the model performs a tool round and the transcript records it
- `generate_echo_app`
  - the model uses `app.install`
  - the generated Lua app is persisted
  - the app can be executed locally through `/api/apps/run`
- `task_event_runtime`
  - a Lua app is installed directly through the admin API with explicit `permissions` and `triggers`
  - an event-driven task is started locally on-device
  - `event.emit` triggers the task without another LLM round-trip
  - the app persists the event payload into the workspace through `espclaw.fs.write(...)` and reads it back through `espclaw.fs.read(...)`
- `vision`
  - the model calls `camera.capture`
  - the device saves a real JPEG into the SD-backed workspace
  - the follow-up Codex run receives that image and returns an image-grounded description that mentions the capture path
- `tool_matrix_full`
  - the bench sends many smaller tool-compliance prompts with YOLO mode enabled
  - each case audits the stored transcript and compares the requested tool names with the expected set
  - this is intended to catch catalog-vs-executor gaps, not just generic chat success
  - cases are intentionally separated so the model cannot satisfy a grouped prompt by only calling the first tool successfully
- `operator_surfaces`
  - the bench runs the same prompt through the `web`, `uart`, and `telegram` operator turn paths
  - each run records memory telemetry and confirms the path still supports a tool-using LLM turn
  - this is intended to catch regressions where one surface overflows or allocates differently than the others
- `semantic_blink_task`
  - the bench asks for a specific hardware behavior with explicit pin/count/timing
  - it validates that `app.install` and `task.start` were both used
  - it then checks the installed app source and running tasks instead of trusting the assistant reply text
- `large_lua_app`
  - the bench asks the live model to install progressively larger Lua apps
  - each threshold validates the saved source length plus a real `app.run` result
  - the current prompt requires top-level helper functions and a top-level `handle(trigger, payload)` entrypoint
  - failures still indicate tool-call emission limits before a confirmed embedded RAM ceiling

## Current Bring-Up Note

The bench is intended for real-device bring-up, not only for final green runs. If a stage fails, the JSON report is still useful because it records:

- the stage that failed
- the exact prompt used
- the raw API response
- the validation that failed

This is the fastest way to turn regressions in the live LLM path into repeatable hardware checks.
