# ESPClaw App Patterns

## Default Rule

Default to:

1. reusable shared logic as a component
2. user-facing logic as an app
3. scheduling as a task or behavior
4. events only when decoupling is actually needed

## Patterns

### Single App Only

Use when the code is local to one feature and should not be reused elsewhere.

Shape:

```text
app.install -> optional task.start or behavior.register
```

### Shared Component Plus Apps

Use when low-level logic should be reused across multiple features.

Shape:

```text
component.install(driver/filter/helper)
app.install(feature_a)
app.install(feature_b)
```

Example:

- `ms5611_driver`
- `weather_station`
- `paragliding_vario`

### Sampler Behavior And Event Consumers

Use when multiple live consumers need the same sensor updates and one sampler should own the hardware.

Shape:

```text
component.install(driver)
app.install(sampler)
behavior.register(sampler autostart)
task.start or behavior.register(consumers listening on emitted events)
```

Only use this if multiple consumers truly need the event fan-out. Otherwise, the simpler `component + app` pattern is better.
