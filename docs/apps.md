# ESPClaw Apps

## Overview

ESPClaw apps are workspace-backed Lua bundles that the firmware can discover and execute at runtime.

Apps are intended to be:

- small
- permissioned
- editable by hand or by ESPClaw itself
- safe enough to keep core firmware logic separate from user automation

ESP32 devices should not load arbitrary native binaries from SD and execute them directly. ESPClaw uses Lua instead, with the firmware acting as the host runtime.

The Lua-enabled firmware now assumes a `4MB` flash layout by default. The older `2MB` / `1MB app partition` layout is too small once the Lua runtime is linked in.

## Layout

Each app lives under:

```text
/workspace/apps/<app_id>/
├── app.json
└── main.lua
```

Example manifest:

```json
{
  "id": "hello_app",
  "version": "0.1.0",
  "title": "Hello App",
  "entrypoint": "main.lua",
  "permissions": ["fs.read", "pwm.write", "adc.read", "imu.read", "temperature.read"],
  "triggers": ["boot", "telegram", "manual"]
}
```

## Script Contract

The Lua entrypoint should define:

```lua
function handle(trigger, payload)
  return "ok"
end
```

`trigger` is a short string such as `boot` or `telegram`.

`payload` is the raw input text associated with that trigger. For Telegram commands this is the text after the app id.

## Available Lua Bindings

The firmware currently injects a global `espclaw` table with:

- `espclaw.log(message)`
- `espclaw.has_permission(permission_name)`
- `espclaw.read_file(relative_path)` requiring `fs.read`
- `espclaw.write_file(relative_path, content)` requiring `fs.write`
- `espclaw.list_apps()`
- `espclaw.gpio.read(pin)` requiring `gpio.read`
- `espclaw.gpio.write(pin, level)` requiring `gpio.write`
- `espclaw.pwm.setup(channel, pin, frequency_hz[, resolution_bits])` requiring `pwm.write`
- `espclaw.pwm.write(channel, duty)` requiring `pwm.write`
- `espclaw.pwm.write_us(channel, pulse_width_us)` requiring `pwm.write`
- `espclaw.pwm.stop(channel)` requiring `pwm.write`
- `espclaw.pwm.state(channel)` requiring `pwm.write`
- `espclaw.servo.attach(channel, pin[, frequency_hz[, resolution_bits]])` requiring `pwm.write`
- `espclaw.servo.write_us(channel, pulse_width_us)` requiring `pwm.write`
- `espclaw.servo.write_norm(channel, value[, min_us[, max_us]])` requiring `pwm.write`
- `espclaw.adc.read_raw(unit, channel)` requiring `adc.read`
- `espclaw.adc.read_mv(unit, channel)` requiring `adc.read`
- `espclaw.i2c.begin(port, sda_pin, scl_pin[, frequency_hz])` requiring `i2c.read`, `i2c.write`, `imu.read`, or `temperature.read`
- `espclaw.i2c.scan(port)` requiring `i2c.read`
- `espclaw.i2c.read_reg(port, address, reg, length)` requiring `i2c.read`
- `espclaw.i2c.write_reg(port, address, reg, bytes)` requiring `i2c.write`
- `espclaw.ppm.begin(channel, pin[, frame_us[, pulse_us]])` requiring `ppm.write`
- `espclaw.ppm.write(channel, {pulse_us, ...})` requiring `ppm.write`
- `espclaw.ppm.state(channel)` requiring `ppm.write`
- `espclaw.temperature.tmp102_c(port[, address])` requiring `temperature.read`
- `espclaw.imu.mpu6050_begin(port[, address])` requiring `imu.read`
- `espclaw.imu.mpu6050_read(port[, address])` requiring `imu.read`
- `espclaw.imu.complementary_roll_pitch(sample, prev_roll, prev_pitch[, alpha[, dt_seconds]])`
- `espclaw.buzzer.tone(channel, pin, frequency_hz, duration_ms[, duty_percent])` requiring `buzzer.play`
- `espclaw.pid.step(setpoint, measurement, integral, previous_error, kp, ki, kd, dt_seconds[, out_min[, out_max]])`
- `espclaw.control.mix_differential(throttle, turn[, out_min[, out_max]])`
- `espclaw.control.mix_quad_x(throttle, roll, pitch, yaw[, out_min[, out_max]])`
- `espclaw.time.ticks_ms()`
- `espclaw.time.sleep_ms(duration_ms)`

The hardware surface is intentionally thin and maps directly to ESP-IDF driver-backed operations so apps can build real control loops without paying for an HTTP-style tool abstraction on every step.

## Triggers

Current triggers:

- `boot`
- `telegram`
- `manual`

Apps declaring `boot` are executed automatically after the workspace is mounted.

The runtime now also supports persistent scheduled execution where the same Lua VM instance is reused across iterations. That allows apps to keep controller state such as integrators, filters, or calibration globals in memory instead of reconstructing them on every trigger.

## Telegram Flow

Current Telegram commands:

- `/apps`
- `/newapp <app_id>`
- `/app <app_id> [payload]`
- `/rmapp <app_id>`

`/newapp` writes a scaffolded manifest and `main.lua` to the workspace. This is the first path toward apps being created by ESPClaw itself.

## App Management Primitives

The runtime now exposes the basic lifecycle operations needed by the admin surface and future tool-call layers:

- read app source
- update app source in place
- remove an app directory recursively
- render installed app metadata as compact JSON
- render app detail JSON including manifest fields and entrypoint source

This keeps app editing in the mounted workspace rather than inside the firmware image and avoids treating dynamic apps as native binaries.

## HTTP Admin Surface

The firmware and host simulator now expose the same app-management API:

- `GET /api/apps`
- `GET /api/apps/detail?app_id=<id>`
- `POST /api/apps/scaffold?app_id=<id>`
- `PUT /api/apps/source?app_id=<id>`
- `POST /api/apps/run?app_id=<id>&trigger=<name>`
- `DELETE /api/apps?app_id=<id>`

Example flow:

```bash
curl -s -X POST 'http://127.0.0.1:8080/api/apps/scaffold?app_id=hello_app'
curl -s 'http://127.0.0.1:8080/api/apps/detail?app_id=hello_app'
curl -s -X PUT 'http://127.0.0.1:8080/api/apps/source?app_id=hello_app' \
  -H 'Content-Type: text/plain' \
  --data-binary $'function handle(trigger, payload)\n  return "hello:" .. payload\nend\n'
curl -s -X POST 'http://127.0.0.1:8080/api/apps/run?app_id=hello_app&trigger=manual' \
  -H 'Content-Type: text/plain' \
  --data 'world'
```

The `run` endpoint uses the request body as the raw payload passed into `handle(trigger, payload)`.

## Persistent Control Loops

The firmware and host simulator now expose a small scheduler API for persistent Lua loops:

- `GET /api/loops`
- `POST /api/loops/start?loop_id=<id>&app_id=<id>&trigger=<name>&period_ms=<n>&iterations=<n>`
- `POST /api/loops/stop?loop_id=<id>`

Behavior:

- `start` opens the app once, keeps the Lua VM alive, and repeatedly calls `handle(trigger, payload)`
- `period_ms` sets the loop cadence
- `iterations=0` means run until explicitly stopped
- `stop` sets `stop_requested` and the loop exits after the active iteration
- loop status JSON reports `active`, `completed`, `iterations_completed`, `last_status`, and `last_result`

Example:

```bash
curl -s -X POST 'http://127.0.0.1:8080/api/loops/start?loop_id=balance&app_id=balance_bot&trigger=manual&period_ms=10&iterations=0' \
  -H 'Content-Type: text/plain' \
  --data 'step'
curl -s 'http://127.0.0.1:8080/api/loops'
curl -s -X POST 'http://127.0.0.1:8080/api/loops/stop?loop_id=balance'
```

## Design Constraints

- App ids are limited to alphanumeric characters, `_`, and `-`
- Entrypoints must stay inside the app directory
- Apps are permission-gated through manifest capabilities
- Hardware bindings stay close to the ESP-IDF drivers, but now include a few typed control drivers where they materially simplify control apps
- Typed helpers currently exist for `TMP102` and `MPU6050`
- PWM targets the LEDC block with four explicit channels exposed to Lua, including servo/ESC pulse helpers
- PPM output targets the RMT block with two explicit channels exposed to Lua
- The runtime is intentionally simpler than general desktop scripting or package managers

## Next Steps

- add more typed IMU and barometer drivers on top of the generic I2C bindings
- add PPM input capture for RC receivers
- add structured event payloads beyond raw text
- add optional WASM runtime later for stronger isolation
