# ESPClaw Hardware Lua

## Purpose

ESPClaw Lua apps can now call native hardware bindings directly instead of bouncing every control step through a high-level tool call loop. This is meant for real embedded work such as PID loops, servo and ESC output, PPM framing, ADC sampling, typed IMU reads, and register-level I2C sensors.

The goal is to keep the policy boundary in the app manifest while keeping the execution path close to the ESP-IDF drivers.

## Current Bindings

### GPIO

- `espclaw.gpio.read(pin)`
- `espclaw.gpio.write(pin, level)`

Permissions:

- `gpio.read`
- `gpio.write`

### PWM

- `espclaw.pwm.setup(channel, pin, frequency_hz[, resolution_bits])`
- `espclaw.pwm.write(channel, duty)`
- `espclaw.pwm.write_us(channel, pulse_width_us)`
- `espclaw.pwm.stop(channel)`
- `espclaw.pwm.state(channel)`

Permission:

- `pwm.write`

Notes:

- The current implementation exposes four PWM channels.
- It uses the LEDC block on real hardware.
- Channel setup is explicit so apps can reserve known channels.

### Servo And ESC Pulses

- `espclaw.servo.attach(channel, pin[, frequency_hz[, resolution_bits]])`
- `espclaw.servo.write_us(channel, pulse_width_us)`
- `espclaw.servo.write_norm(channel, value[, min_us[, max_us]])`

Permission:

- `pwm.write`

These helpers are thin wrappers around the PWM block but use pulse widths instead of raw duty values. That is the intended surface for RC servos and ESCs.

### PPM

- `espclaw.ppm.begin(channel, pin[, frame_us[, pulse_us]])`
- `espclaw.ppm.write(channel, {pulse_us, ...})`
- `espclaw.ppm.state(channel)`

Permission:

- `ppm.write`

Notes:

- The implementation uses the RMT block on real hardware.
- It currently exposes two PPM output channels.
- This is PPM output framing, not RC receiver capture yet.

### Buzzer

- `espclaw.buzzer.tone(channel, pin, frequency_hz, duration_ms[, duty_percent])`

Permission:

- `buzzer.play`

The current implementation is built on the PWM binding.

### ADC

- `espclaw.adc.read_raw(unit, channel)`
- `espclaw.adc.read_mv(unit, channel)`

Permission:

- `adc.read`

The millivolt helper uses ADC calibration on firmware builds when available and falls back to a simple scaling path in the simulator.

### UART

- `espclaw.uart.read(port[, length])`
- `espclaw.uart.write(port, data)`

Permissions:

- `uart.read`
- `uart.write`

Notes:

- The simulator maps `UART0` to the simulator process console.
- `espclaw.uart.write(0, "...")` writes to simulator `stdout`.
- `espclaw.uart.read(0, n)` drains bytes from simulator `stdin`.
- On a normal terminal this is line-buffered by the host TTY, so `uart.read` usually sees typed input after you press Enter.

### I2C

- `espclaw.i2c.begin(port, sda_pin, scl_pin[, frequency_hz])`
- `espclaw.i2c.scan(port)`
- `espclaw.i2c.read_reg(port, address, reg, length)`
- `espclaw.i2c.write_reg(port, address, reg, bytes)`

Permissions:

- `i2c.read`
- `i2c.write`

This is the intended path for temperature sensors, IMUs, and other register-based peripherals.

### Typed Sensors

- `espclaw.temperature.tmp102_c(port[, address])`
- `espclaw.imu.mpu6050_begin(port[, address])`
- `espclaw.imu.mpu6050_read(port[, address])`
- `espclaw.imu.complementary_roll_pitch(sample, prev_roll, prev_pitch[, alpha[, dt_seconds]])`

Permissions:

- `temperature.read`
- `imu.read`

The typed drivers are intentionally narrow. They remove repetitive register decoding for the most common control-stack parts without hiding the underlying I2C surface.

### Control Math, PID, And Time

- `espclaw.pid.step(setpoint, measurement, integral, previous_error, kp, ki, kd, dt_seconds[, out_min[, out_max]])`
- `espclaw.control.mix_differential(throttle, turn[, out_min[, out_max]])`
- `espclaw.control.mix_quad_x(throttle, roll, pitch, yaw[, out_min[, out_max]])`
- `espclaw.time.ticks_ms()`
- `espclaw.time.sleep_ms(duration_ms)`

These helpers are available without extra manifest permissions because they do not mutate external hardware state by themselves.

## Example

```lua
function handle(trigger, payload)
  assert(espclaw.i2c.begin(0, 21, 22, 400000))
  assert(espclaw.imu.mpu6050_begin(0, 0x68))

  local battery_mv = espclaw.adc.read_mv(1, 3)
  local temperature = espclaw.temperature.tmp102_c(0, 0x48)
  local imu = espclaw.imu.mpu6050_read(0, 0x68)
  local roll, pitch = espclaw.imu.complementary_roll_pitch(imu, 0, 0, 0.98, 0.01)
  local drive = espclaw.control.mix_differential(0.4, -0.1, -1, 1)

  local output, integral, error = espclaw.pid.step(
    0,
    roll,
    0,
    0,
    18.0,
    0.4,
    0.2,
    0.01,
    -1.0,
    1.0
  )

  assert(espclaw.servo.attach(0, 12))
  espclaw.servo.write_norm(0, output, 1000, 2000)
  espclaw.servo.attach(1, 13)
  espclaw.servo.write_norm(1, drive.left, 1000, 2000)
  espclaw.servo.attach(2, 14)
  espclaw.servo.write_norm(2, drive.right, 1000, 2000)

  return string.format("battery=%dmV temp=%.1fC roll=%.2f pitch=%.2f error=%.2f", battery_mv, temperature, roll, pitch, error)
end
```

Example manifest permissions for that app:

```json
{
  "permissions": ["pwm.write", "adc.read", "imu.read", "temperature.read"]
}
```

## Simulator Behavior

The host simulator exposes the same Lua API with in-memory state:

- GPIO values are tracked locally
- PWM channel state is tracked locally
- PPM frame output is tracked locally
- ADC reads return simulator-set raw values
- `UART0` is bridged to simulator `stdin`/`stdout`
- I2C devices and register bytes can be seeded by host tests
- Typed `TMP102` and `MPU6050` helpers decode against simulator-seeded registers

That makes the simulator useful for app logic, integration tests, and admin-surface development, but it is still not a cycle-accurate hardware emulator.

## Current Limits

- Only `TMP102` and `MPU6050` have typed helpers right now
- PPM is output only right now
- PWM currently targets LEDC only
- High-rate control loops should still keep the critical math or capture path in native code when jitter matters

Those higher-level drivers should be layered on top of the current primitives instead of replacing them.
