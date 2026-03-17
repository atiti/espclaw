# Real Hardware Bench

`scripts/real_device_bench.py` runs progressive checks against a real ESPClaw device over the admin HTTP API.

The default stage order is:

1. `preflight`
2. `inventory`
3. `hello`
4. `tool_reasoning`
5. `generate_echo_app`
6. `task_event_runtime`

The intent is to move from simple reachability toward live tool inventory, model reasoning, persistent Lua behavior generation, and local autonomous task execution.

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

## Current Bring-Up Note

The bench is intended for real-device bring-up, not only for final green runs. If a stage fails, the JSON report is still useful because it records:

- the stage that failed
- the exact prompt used
- the raw API response
- the validation that failed

This is the fastest way to turn regressions in the live LLM path into repeatable hardware checks.
