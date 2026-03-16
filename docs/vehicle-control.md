# ESPClaw Vehicle Control

## Goal

ESPClaw now exposes enough native control primitives for Lua apps to act as the high-level logic layer for:

- autonomous rovers
- self-balancing robots
- quadcopters and other multirotors

The design principle is simple: timing-sensitive hardware access stays in native code, while Lua owns orchestration, mission logic, and configuration.

## Hardware Blocks

### Actuation

- LEDC-backed PWM for continuous duty outputs
- servo and ESC pulse helpers using microseconds instead of raw duty
- RMT-backed PPM output framing
- buzzer/tone generation for alerts and status

### Sensors

- raw and calibrated ADC reads for battery and analog sensors
- generic I2C register access
- typed `TMP102` temperature helper
- typed `MPU6050` IMU helper

### Control Math

- PID step helper
- complementary roll/pitch estimator
- differential-drive mixer
- quad-X motor mixer

## Recommended Split

For real robots and aircraft, do not put every cycle-critical behavior into Lua.

Recommended split:

- native layer:
  - peripheral access
  - pulse generation
  - sensor register decoding
  - small math kernels used every loop
- Lua layer:
  - mode logic
  - waypoint or mission state
  - configuration
  - safety policy
  - telemetry formatting
  - interaction with Telegram/admin UI/LLM workflows

That keeps the control path predictable while still making the system editable from the SD card.

## Rover Pattern

Typical rover stack:

1. Read battery voltage through `espclaw.adc.read_mv`.
2. Read IMU and wheel or proximity sensors.
3. Compute steering and throttle in Lua.
4. Use `espclaw.control.mix_differential`.
5. Drive ESCs or motor controllers with `espclaw.servo.write_us` or `espclaw.servo.write_norm`.

## Self-Balancing Robot Pattern

Typical balancing stack:

1. Initialize `MPU6050`.
2. Read accel/gyro every loop.
3. Estimate roll or pitch with `espclaw.imu.complementary_roll_pitch`.
4. Feed the angle into `espclaw.pid.step`.
5. Convert output to left/right motor commands with `espclaw.control.mix_differential`.
6. Emit motor pulses through the servo/ESC helpers.

## Quadcopter Pattern

Typical quad stack:

1. Read IMU samples and estimate attitude.
2. Run PID loops for roll, pitch, and yaw in Lua or in future native helpers.
3. Apply `espclaw.control.mix_quad_x`.
4. Map normalized outputs to ESC pulses with `espclaw.servo.write_us` or `espclaw.servo.write_norm`.

`PPM` output is available when a downstream flight controller or RC-style interface expects framed pulse channels instead of per-motor PWM outputs.

## Current Limits

- PPM input capture is not implemented yet.
- Only `TMP102` and `MPU6050` have typed helpers today.
- There is no barometer, magnetometer, or GPS helper yet.
- Hard real-time stabilization loops may eventually need dedicated native tasks instead of pure Lua scheduling.

## What This Makes Possible

With the current blocks, ESPClaw can already serve as:

- a rover brain with differential motor control and sensor feedback
- a balancing robot controller using an MPU6050 and PID loop
- a high-level multirotor controller or actuator testbed with quad mixing and ESC pulse output

The remaining gap is breadth of drivers and tighter real-time loop management, not the basic hardware/control substrate.
