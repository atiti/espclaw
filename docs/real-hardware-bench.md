# Real Hardware Bench

`scripts/real_device_bench.py` runs progressive checks against a real ESPClaw device over the admin HTTP API.

The default stage order is:

1. `preflight`
2. `hello`
3. `tool_reasoning`
4. `generate_echo_app`
5. `generate_battery_app`

The intent is to move from a minimal model reply toward LLM-generated Lua and real hardware use.

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
- `hello`
  - the live LLM loop returns the exact marker `ESPCLAW_BENCH_HI`
- `tool_reasoning`
  - the model performs a tool round and the transcript records it
- `generate_echo_app`
  - the model uses `app.install`
  - the generated Lua app is persisted
  - the app can be executed locally through `/api/apps/run`
- `generate_battery_app`
  - the model inspects hardware
  - the generated Lua app reads the named `battery` ADC channel
  - the result is validated from a real app run

## Current Bring-Up Note

The bench is intended for real-device bring-up, not only for final green runs. If a stage fails, the JSON report is still useful because it records:

- the stage that failed
- the exact prompt used
- the raw API response
- the validation that failed

This is the fastest way to turn regressions in the live LLM path into repeatable hardware checks.
