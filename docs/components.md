# ESPClaw Components

## Overview

Components are reusable Lua modules with metadata. They are the intended way to share drivers, filters, and helper logic across multiple apps.

Each component lives under:

```text
/workspace/components/<component_id>/
├── component.json
└── module.lua
```

The runtime also publishes the module source into `/workspace/lib/...` so apps can load it with `require(...)`.

## When To Use Components

Use a component when:

- the Lua code should be reused by multiple apps
- the logic is low-level or infrastructural
- the code is a driver, filter, mixer, parser, or sensor adapter

Do not use a component for scheduling or autostart. That belongs to:

- apps for product logic
- tasks for live schedules
- behaviors for persisted schedules
- events for decoupled signaling

## Example: MS5611

Recommended shape:

- component: `ms5611_driver` exporting `require("sensors.ms5611")`
- app: `weather_station`
- app: `paragliding_vario`
- task or behavior: whichever schedule each app needs

Only introduce events if you want a separate sampler app that owns the sensor and fans out readings to multiple consumers.

## APIs

HTTP:

- `GET /api/components`
- `GET /api/components/detail?component_id=<id>`
- `POST /api/components/install?component_id=<id>&module=<module_name>[&title=<title>&summary=<summary>&version=<version>]`
- `DELETE /api/components?component_id=<id>`

Model tools:

- `component.list`
- `component.install`
- `component.remove`
