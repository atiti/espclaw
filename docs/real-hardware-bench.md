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

- `tool_matrix_full`
  - runs many smaller, audited prompts instead of one giant sweep
  - checks which tool names the model actually requested in the transcript
  - is the preferred way to measure real tool-call coverage on hardware
- `large_lua_app`
  - asks the live model to build progressively larger Lua apps through `app.install`
  - verifies the installed source size and the app's runtime behavior
  - currently exposes model tool-call compliance limits before it hits a proven device RAM ceiling on `esp32cam`

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
- `large_lua_app`
  - the bench asks the live model to install progressively larger Lua apps
  - each threshold validates the saved source length plus a real `app.run` result
  - failures currently indicate tool-call compliance or parser issues before a confirmed embedded RAM ceiling

## Current Bring-Up Note

The bench is intended for real-device bring-up, not only for final green runs. If a stage fails, the JSON report is still useful because it records:

- the stage that failed
- the exact prompt used
- the raw API response
- the validation that failed

This is the fastest way to turn regressions in the live LLM path into repeatable hardware checks.
