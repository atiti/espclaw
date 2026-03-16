# Board Configuration

ESPClaw uses two layers for hardware targeting:

- board profiles for platform constraints such as storage backend, provisioning mode, camera support, and CPU core count
- board descriptors for the actual wiring of pins, buses, and ADC channels

That keeps firmware binaries reusable across a class of boards while still letting the workspace define the real pinout.

## Workspace File

Board descriptors live at:

```text
/workspace/config/board.json
```

If the file is missing, ESPClaw falls back to the built-in descriptor for the active board profile.

## Built-In Variants

Current built-in variants:

- `generic_esp32s3`
- `ai_thinker_esp32cam`
- `generic_esp32c3`
- `seeed_xiao_esp32c3`

The built-in descriptor is selected from the board profile, then overridden by `board.json`.

The admin UI and simulator expose those built-ins through:

- `GET /api/board/presets`
- `POST /api/board/apply?variant_id=<id>`
- `GET /api/board/config`
- `PUT /api/board/config`

That makes it practical to ship one firmware binary per board class and finish the exact wiring at deploy time instead of rebuilding per carrier board.

## JSON Format

Example:

```json
{
  "variant": "seeed_xiao_esp32c3",
  "pins": {
    "servo_main": 4,
    "ppm_out": 5,
    "buzzer": 6
  },
  "i2c": {
    "default": {
      "port": 0,
      "sda": 6,
      "scl": 7,
      "frequency_hz": 400000
    }
  },
  "uart": {
    "console": {
      "port": 0,
      "tx": 21,
      "rx": 20,
      "baud_rate": 115200
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

Supported top-level keys:

- `variant`
- `pins`
- `i2c.default`
- `uart.console`
- `adc.battery`

## Lua Access

Lua apps can use named resources instead of hard-coded GPIO numbers:

```lua
local board = espclaw.board.describe()
local servo_pin = espclaw.board.pin("servo_main")
local battery_mv = espclaw.adc.read_named_mv("battery")
espclaw.i2c.begin_board("default")
espclaw.servo.attach("servo_main", 50)
espclaw.ppm.begin("ppm_out")
```

Board lookup helpers:

- `espclaw.board.describe()`
- `espclaw.board.pin(name)`
- `espclaw.board.i2c(name)`
- `espclaw.board.uart(name)`
- `espclaw.board.adc(name)`

Hardware bindings that accept aliases now include GPIO, PWM, servo, buzzer, and PPM.

## Admin API

The active resolved descriptor is exposed through:

```text
GET /api/board
```

This returns the current variant, descriptor source, named pins, buses, ADC channels, and task-placement policy.

The config editor route returns the raw workspace JSON plus provenance:

```text
GET /api/board/config
```

The preset listing route only returns variants valid for the current board profile:

```text
GET /api/board/presets
```

## Multicore Policy

ESPClaw now applies a small task-placement policy derived from the board profile:

- single-core boards: no affinity
- dual-core boards: admin server and Telegram on core `0`, control loops on core `1`

The board descriptor is independent from that policy, but both are reported together through the admin API because they define how the firmware will behave on a specific board.
