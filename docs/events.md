# ESPClaw Events

## Overview

ESPClaw now has three layers for local autonomy:

1. Lua apps define logic.
2. Tasks and behaviors schedule that logic.
3. Hardware event watches inject local events when the physical world changes.

That lets the LLM write behaviors once and then let the device react locally instead of paying for a cloud round-trip on every hardware change.

## Current Event Sources

The first automatic hardware event producers are:

- `uart`
  - watches a UART port for incoming bytes
  - emits the configured event name with the received text payload
- `adc_threshold`
  - polls an ADC channel
  - emits when the sampled value crosses above or below the configured threshold

These are intentionally simple, deterministic primitives that map well to embedded control logic.

## Runtime Model

Event watches emit named events into the existing task runtime.

An event-driven task can bind to that event:

```lua
function on_uart(payload)
  return "rx:" .. payload
end

function on_sensor(payload)
  return "sensor:" .. payload
end
```

Then the runtime can keep that task alive without consulting the LLM again.

## Model Tools

The model now has direct watch-management tools:

- `event.watch_list`
- `event.watch_add`
- `event.watch_remove`

Examples:

```json
{"watch_id":"uart_console","kind":"uart","event_name":"uart","port":0}
```

```json
{"watch_id":"battery_high","kind":"adc_threshold","event_name":"sensor","unit":1,"channel":3,"threshold":1000,"interval_ms":25}
```

## Lua API

Lua apps can manage watches locally through:

- `espclaw.events.list()`
- `espclaw.events.watch_uart(watch_id[, event_name[, port]])`
- `espclaw.events.watch_adc_threshold(watch_id, unit, channel, threshold[, event_name[, interval_ms]])`
- `espclaw.events.remove_watch(watch_id)`

These calls require `task.control`.

## Current Limits

- UART watch payloads are treated as text.
- ADC watches currently emit only on threshold crossings, not on every sample.
- There are no ISR-backed GPIO or sensor-specific producers yet.
- Real camera, IMU interrupt, and richer sensor-originated event producers are still follow-on work.
