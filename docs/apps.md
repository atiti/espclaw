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
├── main.lua
└── lib/
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

The runtime now resolves trigger handlers in this order:

1. `on_<trigger>(payload)`
2. `on_event(trigger, payload)`
3. `handle(trigger, payload)`

So an event-driven app can stay explicit:

```lua
function on_timer(payload)
  return "tick:" .. payload
end

function on_sensor(payload)
  return "sensor:" .. payload
end
```

## Reusable Modules

Lua apps can now use `require(...)` for shared components.

The runtime prepends these search paths to `package.path`:

- `/workspace/lib/?.lua`
- `/workspace/lib/?/init.lua`
- `/workspace/apps/<app_id>/lib/?.lua`
- `/workspace/apps/<app_id>/lib/?/init.lua`
- `/workspace/apps/<app_id>/?.lua`

That allows a shared workspace library plus app-local helpers.

Example:

```text
/workspace/lib/sensors/ultrasonic.lua
/workspace/apps/rover/lib/control/pid.lua
```

```lua
local ultrasonic = require("sensors.ultrasonic")
local pid = require("control.pid")
```

This is the intended path for nested Lua components such as drivers, filters, mixers, and sensor adapters that need to be reused by multiple tasks or apps.

## Available Lua Bindings

The firmware currently injects a global `espclaw` table with:

- `espclaw.log(message)`
- `espclaw.has_permission(permission_name)`
- `espclaw.read_file(relative_path)` requiring `fs.read`
- `espclaw.write_file(relative_path, content)` requiring `fs.write`
- `espclaw.fs.read(relative_path)` requiring `fs.read`
- `espclaw.fs.write(relative_path, content)` requiring `fs.write`
- `espclaw.list_apps()`
- `espclaw.hardware.list()`
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
- `espclaw.camera.capture([filename])` requiring `camera.capture`
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
- `espclaw.events.emit(name[, payload])` requiring `task.control`
- `espclaw.events.list()` requiring `task.control`
- `espclaw.events.watch_uart(watch_id[, event_name[, port]])` requiring `task.control`
- `espclaw.events.watch_adc_threshold(watch_id, unit, channel, threshold[, event_name[, interval_ms]])` requiring `task.control`
- `espclaw.events.remove_watch(watch_id)` requiring `task.control`

The hardware surface is intentionally thin and maps directly to ESP-IDF driver-backed operations so apps can build real control loops without paying for an HTTP-style tool abstraction on every step.

## Triggers

Common triggers:

- `boot`
- `telegram`
- `manual`
- `timer`
- `sensor`
- `uart`

Trigger names are not hard-coded to that set. Tasks and tools can emit any short trigger string that the manifest declares.

Apps declaring `boot` are executed automatically after the workspace is mounted.

The runtime now also supports persistent scheduled execution where the same Lua VM instance is reused across iterations. That allows apps to keep controller state such as integrators, filters, or calibration globals in memory instead of reconstructing them on every trigger.

There are now two task styles:

- `periodic`: the runtime calls the chosen trigger every `period_ms`
- `event`: the runtime waits for `event.emit` and then calls the chosen trigger with the emitted payload

ESPClaw now also has hardware-originated event watches that sit underneath those event tasks:

- `uart` watches emit when bytes arrive on a watched UART port
- `adc_threshold` watches emit when an ADC channel crosses a threshold boundary

That lets the LLM create local autonomy in two layers:

1. write or install a Lua app
2. register behavior/task schedules and hardware event watches that keep running without another cloud round-trip

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

The model tool loop can now use those primitives directly through:

- `app.install`
- `app.run`
- `app.remove`

`app.install` accepts Lua source plus optional `permissions_csv` and `triggers_csv`, which is the intended path for the model to generate persistent behaviors and then attach them to tasks.

The model can now also skip the two-step flow and use first-class behavior tools:

- `behavior.list`
- `behavior.register`
- `behavior.start`
- `behavior.stop`
- `behavior.remove`

`behavior.register` persists the runtime definition under `/workspace/behaviors/<behavior_id>.json`. If `source` is included, it will also scaffold or update the referenced app before saving the behavior.

For hardware-originated events, the model can now also manage watches directly:

- `event.watch_list`
- `event.watch_add`
- `event.watch_remove`

The first implementation supports:

- `kind="uart"` with `port`
- `kind="adc_threshold"` with `unit`, `channel`, `threshold`, and optional `interval_ms`

Those watches emit local named events into the task runtime, so an event-driven app can react without invoking the LLM again.

## HTTP Admin Surface

The firmware and host simulator now expose the same app-management API:

- `GET /api/apps`
- `GET /api/apps/detail?app_id=<id>`
- `POST /api/apps/scaffold?app_id=<id>[&title=<title>&permissions=<csv>&triggers=<csv>]`
- `PUT /api/apps/source?app_id=<id>`
- `POST /api/apps/run?app_id=<id>&trigger=<name>`
- `DELETE /api/apps?app_id=<id>`
- `GET /api/tasks`
- `POST /api/tasks/start?task_id=<id>&app_id=<id>&schedule=<periodic|event>&trigger=<name>&period_ms=<n>&iterations=<n>`
- `POST /api/tasks/stop?task_id=<id>`
- `POST /api/events/emit?name=<event>`
- `GET /api/behaviors`
- `POST /api/behaviors/register?behavior_id=<id>&app_id=<id>&schedule=<periodic|event>&trigger=<name>&period_ms=<n>&iterations=<n>&autostart=<0|1>`
- `POST /api/behaviors/start?behavior_id=<id>`
- `POST /api/behaviors/stop?behavior_id=<id>`
- `DELETE /api/behaviors?behavior_id=<id>`

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

## Persistent Tasks

The firmware and host simulator now expose a small scheduler API for persistent Lua tasks:

- `GET /api/tasks`
- `POST /api/tasks/start?task_id=<id>&app_id=<id>&schedule=<periodic|event>&trigger=<name>&period_ms=<n>&iterations=<n>`
- `POST /api/tasks/stop?task_id=<id>`
- `POST /api/events/emit?name=<event>`

Behavior:

- `start` opens the app once and keeps the Lua VM warm
- `schedule=periodic` repeatedly calls the chosen trigger at the requested cadence
- `schedule=event` waits for `POST /api/events/emit` or `event.emit`
- `iterations=0` means run until explicitly stopped
- `stop` sets `stop_requested` and the worker exits cleanly
- task status JSON reports `active`, `completed`, `iterations_completed`, `events_received`, `last_status`, and `last_result`

## Persisted Behaviors

Behaviors are the durable layer on top of tasks.

- a task is a live worker with a warm Lua VM
- a behavior is the saved runtime definition for how that task should be recreated

Each behavior stores:

- `behavior_id`
- `app_id`
- `schedule`
- `trigger`
- `payload`
- `period_ms`
- `max_iterations`
- `autostart`

Autostart behaviors are started automatically after workspace mount and boot-app execution.

Example:

```bash
curl -s -X POST 'http://127.0.0.1:8080/api/behaviors/register?behavior_id=avoidance&app_id=rover_app&schedule=event&trigger=sensor&period_ms=20&iterations=0&autostart=1' \
  -H 'Content-Type: text/plain' \
  --data 'distance'
curl -s -X POST 'http://127.0.0.1:8080/api/behaviors/start?behavior_id=avoidance'
curl -s 'http://127.0.0.1:8080/api/behaviors'
curl -s -X POST 'http://127.0.0.1:8080/api/events/emit?name=sensor' \
  -H 'Content-Type: text/plain' \
  --data 'distance_cm=18'
```

Example:

```bash
curl -s -X POST 'http://127.0.0.1:8080/api/tasks/start?task_id=balance&app_id=balance_bot&schedule=periodic&trigger=timer&period_ms=10&iterations=0' \
  -H 'Content-Type: text/plain' \
  --data 'step'
curl -s 'http://127.0.0.1:8080/api/tasks'
curl -s -X POST 'http://127.0.0.1:8080/api/tasks/stop?task_id=balance'
curl -s -X POST 'http://127.0.0.1:8080/api/tasks/start?task_id=ultra_watch&app_id=balance_bot&schedule=event&trigger=sensor&iterations=0'
curl -s -X POST 'http://127.0.0.1:8080/api/events/emit?name=sensor' \
  -H 'Content-Type: text/plain' \
  --data 'distance_cm=42'
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
